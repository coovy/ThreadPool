#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

//生成名为tp的命名空间
namespace tp {
enum class ThreadPoolStatus { CLOSED, RUNNING, WAITING, PAUSED };

//模板类
template <typename T>
struct Block {
 private:
  T start_, end_;
  size_t block_size_ = 0;
  size_t num_ = 0;
  size_t total_size_ = 0;

 public:
  static_assert(std::is_integral_v<T>, "T must be an integral type.");
  Block(const T s, const T e, size_t num) : start_(s), end_(e), num_(num) {
    if (start_ >= end_) {
      throw std::invalid_argument("start_ must be less than or equal to end_.");
    }
    //强制类型转换，将end-start类型转换为size_t类型
    total_size_ = static_cast<size_t>(end_ - start_);
    if (total_size_ < num_) {
      throw std::invalid_argument(
          "num_ must be less than or equal to total_size_.");
    }
    block_size_ = total_size_ / num_;
    //如果end=start，则block_size=1,num_=1
    if (block_size_ == 0) {
      block_size_ = 1;
      num_ = block_size_;
    }
  }

  [[nodiscard]] T get_block_start(const size_t i) const {
    if (i >= num_) throw std::runtime_error("Block index out of range.");
    return static_cast<T>(i * block_size_) + start_;
  }

  [[nodiscard]] T get_block_end(const size_t i) const {
    if (i >= num_) throw std::runtime_error("Block index out of range.");
    return (i == num_ - 1) ? end_
                           : static_cast<T>((i + 1) * block_size_) + start_;
  }

  [[nodiscard]] size_t get_num_blocks() const noexcept { return num_; }

  Block(const Block&) = default;
  Block& operator=(const Block&) = default;
};

class ThreadPool {
 public:
  ThreadPool(size_t count = std::thread::hardware_concurrency(),
             bool destroy_idle_ = false)
      : threads_count_(count), destroy_idle_(destroy_idle_) {
    threads_.resize(THREADS_MAX_ + 1);
    create_pool(count);
    manager_ = std::thread(&ThreadPool::manager_call, this);
  }

  ~ThreadPool() {
    wait_until_done();
    destroy_pool();
  }

  template <typename F, typename... Args>
  decltype(auto) push(F&& f, Args&&... args) {
    if (is_closed())
      throw std::runtime_error("Error: Adding tasks_ on a closed thread pool.");
    using return_type = std::invoke_result_t<F, Args...>;  //获得F的返回类型
    // std::make_shared创建一个智能指针，指向一个packaged_task对象，
    // packaged_task对象的返回值类型是return_type
    // packaged_task对象由std::bind创建，std:bind绑定f和args
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    //有一个task，放在线程里运行，get_future存储它的返回值，然后再通过get得到它的返回值
    auto res = task->get_future();
    {
      //锁住任务队列，可以实现自动解锁
      std::lock_guard<std::mutex> lk(tasks_mtx_);
      tasks_.push_back([task]() { (*task)(); });
      tasks_count_++;
    }
    task_avaliable_cv_.notify_one();  //唤醒一个等待的线程
    return res;
  }

  template <typename F, typename T>
  decltype(auto) push_loop(F&& f, const T start_, const T end_,
                           const size_t num_ = 0) {
    static_assert(std::is_integral_v<T>, "Error: Loop ranges is non-integral.");
    if (start_ >= end_) throw std::runtime_error("Error: Improper loop range.");
    //  std::format("Error: Improper loop range from {} to {}.", start_, end_)
    //      .c_str());
    Block bk(start_, end_, num_ ? num_ : std::thread::hardware_concurrency());
    for (size_t i = 0; i < bk.get_num_blocks(); ++i) {
      push(std::forward<F>(f), bk.get_block_start(i), bk.get_block_end(i));
    }
  }

  [[nodiscard]] size_t get_threads_count() const noexcept {
    return threads_count_;
  }

  [[nodiscard]] size_t get_threads_running() const noexcept {
    return threads_running_;
  }

  [[nodiscard]] size_t get_tasks_count() const noexcept { return tasks_count_; }

  [[nodiscard]] size_t get_tasks_total() const noexcept {
    return tasks_count_ + threads_running_;
  }

  void pause() {
    if (is_paused()) return;
    if (!is_running())
      throw std::runtime_error("Error: Pausing a not-running thread pool.");
    status_ = ThreadPoolStatus::PAUSED;
  }

  void resume() {  //
    if (is_running()) return;
    if (!is_paused())
      throw std::runtime_error("Error: Resuming a not-paused thread pool.");
    status_ = ThreadPoolStatus::RUNNING;
    task_avaliable_cv_.notify_all();
  }

  void clear() {
    std::lock_guard<std::mutex> lk(tasks_mtx_);
    tasks_.clear();
    tasks_count_ = 0;
  }

  void wait_until_done() {
    if (is_closed() || is_paused()) return;
    std::unique_lock<std::mutex> lk(tasks_mtx_);
    status_ = ThreadPoolStatus::WAITING;
    // wait传入两个参数，1：锁，2：阻塞条件（当它为false时，阻塞）
    pool_done_cv_.wait(lk, [this] { return !threads_running_ && is_empty(); });
  }

  template <typename _Rep, typename _Period>
  bool wait_for(const chrono::duration<_Rep, _Period>& _rel_time) {
    if (is_closed() || is_paused()) return true;
    std::unique_lock<std::mutex> lk(tasks_mtx_);
    status_ = ThreadPoolStatus::WAITING;
    bool res = pool_done_cv_.wait_for(
        lk, _rel_time, [this] { return !threads_running_ && is_empty(); });
    return res;
  }

  bool is_running() const noexcept {
    return status_ == ThreadPoolStatus::RUNNING;
  }

  bool is_waiting() const noexcept {
    return status_ == ThreadPoolStatus::WAITING;
  }

  bool is_paused() const noexcept {
    return status_ == ThreadPoolStatus::PAUSED;
  }

  bool is_closed() const noexcept {
    return status_ == ThreadPoolStatus::CLOSED;
  }

  bool is_empty() const noexcept { return tasks_count_ == 0; }

 private:
  void create_pool(size_t count) {
    status_ = ThreadPoolStatus::RUNNING;
    for (size_t i = 0; i < count; ++i) {
      threads_[i] = std::thread(&ThreadPool::dispatch, this);
    }
  }

  void destroy_pool() {
    status_ = ThreadPoolStatus::CLOSED;
    task_avaliable_cv_.notify_all();
    if (manager_.joinable()) manager_.join();
    for (size_t i = 0; i < threads_count_; ++i) {
      if (threads_[i].joinable()) threads_[i].join();
    }
  }

  void dispatch() {
    function<void()> task;
    while (true) {
      std::unique_lock<std::mutex> lk(tasks_mtx_);
      task_avaliable_cv_.wait(lk, [this] {
        // (非空 || 关闭 || 线程销毁 > 0) && 非暂停  满足时不阻塞，不满足时阻塞
        return (!is_empty() || is_closed() || threads_destroy_ > 0) &&
               !is_paused();
      });

      if (threads_destroy_ > 0 && is_empty() && !is_closed()) {
        --threads_destroy_;
        ids.emplace(std::this_thread::get_id());
        break;
      }

      if (is_closed() && is_empty()) break;

      task = std::move(tasks_.front());  // get the first task
      tasks_.pop_front();                // delete the first task
      --tasks_count_;
      lk.unlock();
      ++threads_running_;
      task();
      --threads_running_;

      if (is_waiting() && !threads_running_ && is_empty()) {
        pool_done_cv_.notify_all();
      }
    }
  }

  void manager_call() {
    while (true) {
      if (is_closed()) break;
      // std::this_thread::sleep_for(100ms);
      if (tasks_count_ > threads_count_ && threads_count_ < THREADS_MAX_) {
        size_t add = min<size_t>(THREADS_ADD_, THREADS_MAX_ - threads_count_);
        size_t j = 0;
        std::lock_guard<std::mutex> lk(tasks_mtx_);
        for (size_t i = 0; i < THREADS_MAX_ && j < add && !ids.empty(); ++i) {
          if (!threads_[i].joinable()) continue;
          auto id = threads_[i].get_id();
          if (ids.count(id)) {
            threads_[i].join();
            threads_[i] = std::thread(&ThreadPool::dispatch, this);
            ids.erase(id);
          }
        }
        for (size_t i = 0; i < THREADS_MAX_ && j < add; ++i) {
          if (!threads_[i].joinable()) {
            threads_[i] = std::thread(&ThreadPool::dispatch, this);
            ++j;
            ++threads_count_;
          }
        }
      }

      {
        if (!ids.empty()) {
          std::lock_guard<std::mutex> lk(tasks_mtx_);
          for (size_t i = 0; i < THREADS_MAX_ && !ids.empty(); ++i) {
            if (!threads_[i].joinable()) continue;
            auto id = threads_[i].get_id();
            if (ids.count(id)) {
              threads_[i].join();
              ids.erase(id);
              --threads_count_;
            }
          }
        }
      }

      if (destroy_idle_) {
        if (threads_running_ * 2 < threads_count_ &&
            threads_count_ > THREADS_MIN_) {
          size_t add = min<size_t>(THREADS_ADD_, THREADS_MAX_ - threads_count_);
          this->threads_destroy_ = add;
        }
      }
    }
  }

 private:
  // A condition variable to notify worker threads_ that a task is available.
  condition_variable task_avaliable_cv_ = {};

  // A condition variable to notify the main thread that the all tasks_ have
  // been done.
  condition_variable pool_done_cv_ = {};

  // A queue of tasks_.
  deque<function<void()>> tasks_ = {};

  // A hash set of thread ids.
  unordered_set<std::thread::id> ids = {};

  // The total number of tasks_ in queue excluding the running tasks_.
  std::atomic<size_t> tasks_count_ = 0;

  // A mutex to synchronize access to the tasks_ queue.(同步的)
  mutex tasks_mtx_ = {};

  // A array of threads_.
  vector<std::thread> threads_;

  // A daemon thread to manage the thread pool.
  std::thread manager_;

  // The max number of threads_ that can be created.
  static inline size_t THREADS_MAX_ = 4 * thread::hardware_concurrency();
  // The min number of threads_ that can be created.
  static inline size_t THREADS_MIN_ =
      min<size_t>(2, thread::hardware_concurrency() / 2);

  // The interval of adding or destroy threads_.
  size_t THREADS_ADD_ = 4;

  // The total number of threads_ in the pool.
  std::atomic<size_t> threads_count_ = 0;

  // The number of currently running threads_.
  std::atomic<size_t> threads_running_ = 0;

  // The number of threads_ to destroy.
  std::atomic<size_t> threads_destroy_ = 0;

  // A flag of the thread-pool state.
  std::atomic<ThreadPoolStatus> status_ = ThreadPoolStatus::CLOSED;

  bool destroy_idle_ = false;
};
}  // namespace tp

#include <iostream>
#include <thread>

#include "thread_pool.h"

using namespace std;

long long rowSum(vector<int> &v) {
  long long sum = 0;
  for (auto i = 0u; i < v.size(); i++) {
    sum += v[i];
  }
  return sum;
}

std::chrono::high_resolution_clock::time_point start() {
  return std::chrono::high_resolution_clock::now();
}

long long duration(std::chrono::high_resolution_clock::time_point t1) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
             std::chrono::high_resolution_clock::now() - t1)
      .count();
}

int main() {
  auto tp = tp::ThreadPool();
  vector<std::future<long long>> res;
  vector<vector<int>> v(10000, vector<int>(100000, 1));

  auto t1 = start();

  for (auto i = 0u; i < v.size(); i++) {
    res.push_back(tp.push(rowSum, v[i]));
  }
  tp.wait_until_done();

  cout << res[0].get() << endl;
  cout << res[9].get() << endl;
  cout << res[99].get() << endl;
  cout << res[999].get() << endl;
  cout << "Hello, World!" << endl;

  cout << "Multi threads execution time: " << duration(t1) << " ms" << endl;

  vector<long long> res1;

  auto t2 = start();
  for (auto i = 0u; i < v.size(); i++) {
    res1.push_back(rowSum(v[i]));
  }

  cout << "Single thread execution time: " << duration(t2) << " ms" << endl;
  return 0;
}

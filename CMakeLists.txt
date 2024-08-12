cmake_minimum_required(VERSION 3.20) 

project(ThreadPool)

set(CMAKE_CXX_STANDARD 20)

set(SOURCE_FILES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_executable(main ${SOURCE_FILES})

include_directories(${CMAKE_SOURCE_DIR}/include)

set(CMAKE_VERBOSE_MAKEFILE TRUE)
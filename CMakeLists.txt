cmake_minimum_required(VERSION 3.14)
project(LockFreeSkipList CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(benchmark src/main.cpp)
target_link_libraries(benchmark PRIVATE Threads::Threads)

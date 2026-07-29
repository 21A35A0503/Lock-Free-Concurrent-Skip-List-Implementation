cmake_minimum_required(VERSION 3.14)
project(LockFreeSkipList CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags for multi-threaded benchmarking
if(MSVC)
    add_compile_options(/W4 /O2)
else()
    add_compile_options(-Wall -Wextra -O3 -pthread)
endif()

add_executable(skiplist_bench main.cpp)

find_package(Threads REQUIRED)
target_link_libraries(skiplist_bench PRIVATE Threads::Threads)

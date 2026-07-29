#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include "lock_free_skip_list.hpp"
#include "epoch_manager.hpp"

void benchmark_thread(LockFreeSkipList<int, int>& list, int thread_id, int ops_per_thread) {
    for (int i = 0; i < ops_per_thread; ++i) {
        int key = (thread_id * ops_per_thread) + i;
        list.insert(key, key * 10);
        list.contains(key);
        if (i % 2 == 0) {
            list.remove(key);
        }
    }
}

int main() {
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 50000;

    LockFreeSkipList<int, int> list;
    std::vector<std::thread> threads;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(benchmark_thread, std::ref(list), i, ops_per_thread);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;

    std::cout << "Completed " << (num_threads * ops_per_thread) 
              << " operations in " << duration.count() << " ms" << std::endl;

    return 0;
}

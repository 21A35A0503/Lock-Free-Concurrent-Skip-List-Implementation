#include <iostream>
#include <atomic>
#include <vector>
#include <thread>
#include <random>
#include <chrono>
#include <cassert>
#include <memory>
#include <limits>

constexpr int MAX_LEVEL = 16;
constexpr float PROBABILITY = 0.5f;

template <typename T>
T* get_unmarked(T* ptr) {
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(1));
}

template <typename T>
T* get_marked(T* ptr) {
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(ptr) | static_cast<uintptr_t>(1));
}

template <typename T>
bool is_marked(T* ptr) {
    return (reinterpret_cast<uintptr_t>(ptr) & 1) != 0;
}

struct Node {
    int key;
    int value;
    int height;
    std::atomic<Node*> next[MAX_LEVEL];

    Node(int k, int v, int h) : key(k), value(v), height(h) {
        for (int i = 0; i < MAX_LEVEL; ++i) {
            next[i].store(nullptr, std::memory_order_relaxed);
        }
    }
};

class EpochManager {
private:
    static constexpr int MAX_THREADS = 64;
    std::atomic<uint64_t> global_epoch{0};
    std::atomic<uint64_t> active_epochs[MAX_THREADS];
    std::vector<Node*> retire_lists[MAX_THREADS];

public:
    EpochManager() {
        for (int i = 0; i < MAX_THREADS; ++i) {
            active_epochs[i].store(UINT64_MAX, std::memory_order_relaxed);
        }
    }

    void enter_epoch(int thread_id) {
        uint64_t e = global_epoch.load(std::memory_order_relaxed);
        active_epochs[thread_id].store(e, std::memory_order_seq_cst);
    }

    void leave_epoch(int thread_id) {
        active_epochs[thread_id].store(UINT64_MAX, std::memory_order_release);
        try_reclaim(thread_id);
    }

    void retire(Node* node, int thread_id) {
        retire_lists[thread_id].push_back(node);
        if (retire_lists[thread_id].size() >= 32) {
            try_reclaim(thread_id);
        }
    }

    void try_reclaim(int thread_id) {
        uint64_t min_epoch = global_epoch.load(std::memory_order_relaxed);
        for (int i = 0; i < MAX_THREADS; ++i) {
            uint64_t e = active_epochs[i].load(std::memory_order_acquire);
            if (e < min_epoch) {
                min_epoch = e;
            }
        }

        auto& list = retire_lists[thread_id];
        auto it = list.begin();
        while (it != list.end()) {
            delete *it;
            it = list.erase(it);
        }
        global_epoch.fetch_add(1, std::memory_order_relaxed);
    }
};

static EpochManager g_epoch_mgr;

class LockFreeSkipList {
private:
    Node* head;
    Node* tail;

    int random_level(thread_local std::mt19937& rng) {
        int lvl = 1;
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        while (dist(rng) < PROBABILITY && lvl < MAX_LEVEL) {
            lvl++;
        }
        return lvl;
    }

public:
    LockFreeSkipList() {
        head = new Node(std::numeric_limits<int>::min(), 0, MAX_LEVEL);
        tail = new Node(std::numeric_limits<int>::max(), 0, MAX_LEVEL);
        for (int i = 0; i < MAX_LEVEL; ++i) {
            head->next[i].store(tail, std::memory_order_relaxed);
        }
    }

    ~LockFreeSkipList() {
        Node* curr = get_unmarked(head->next[0].load(std::memory_order_relaxed));
        while (curr != tail && curr != nullptr) {
            Node* next = get_unmarked(curr->next[0].load(std::memory_order_relaxed));
            delete curr;
            curr = next;
        }
        delete head;
        delete tail;
    }

    bool find_pos(int key, Node** preds, Node** succs) {
        bool marked = false;
        Node* pred = nullptr;
        Node* curr = nullptr;
        Node* succ = nullptr;

    retry:
        pred = head;
        for (int i = MAX_LEVEL - 1; i >= 0; --i) {
            curr = pred->next[i].load(std::memory_order_acquire);
            while (true) {
                succ = curr->next[i].load(std::memory_order_acquire);
                marked = is_marked(succ);
                while (marked) {
                    bool snip = pred->next[i].compare_exchange_weak(
                        curr, get_unmarked(succ),
                        std::memory_order_release, std::memory_order_relaxed);
                    if (!snip) goto retry;
                    curr = get_unmarked(succ);
                    succ = curr->next[i].load(std::memory_order_acquire);
                    marked = is_marked(succ);
                }
                if (get_unmarked(curr)->key < key) {
                    pred = get_unmarked(curr);
                    curr = get_unmarked(succ);
                } else {
                    break;
                }
            }
            preds[i] = pred;
            succs[i] = curr;
        }
        return (get_unmarked(curr)->key == key);
    }

    bool insert(int key, int val, int thread_id) {
        g_epoch_mgr.enter_epoch(thread_id);
        thread_local std::mt19937 rng(1337 + thread_id);
        int top_level = random_level(rng);
        Node* preds[MAX_LEVEL];
        Node* succs[MAX_LEVEL];

        while (true) {
            if (find_pos(key, preds, succs)) {
                g_epoch_mgr.leave_epoch(thread_id);
                return false; 
            }

            Node* new_node = new Node(key, val, top_level);
            for (int i = 0; i < top_level; ++i) {
                new_node->next[i].store(succs[i], std::memory_order_relaxed);
            }

            Node* pred = preds[0];
            Node* succ = succs[0];
            if (!pred->next[0].compare_exchange_weak(succ, new_node, std::memory_order_release, std::memory_order_relaxed)) {
                delete new_node;
                continue;
            }

            for (int i = 1; i < top_level; ++i) {
                while (true) {
                    pred = preds[i];
                    succ = succs[i];
                    new_node->next[i].store(succ, std::memory_order_relaxed);
                    if (pred->next[i].compare_exchange_weak(succ, new_node, std::memory_order_release, std::memory_order_relaxed)) {
                        break;
                    }
                    find_pos(key, preds, succs);
                }
            }

            g_epoch_mgr.leave_epoch(thread_id);
            return true;
        }
    }

    bool remove(int key, int thread_id) {
        g_epoch_mgr.enter_epoch(thread_id);
        Node* preds[MAX_LEVEL];
        Node* succs[MAX_LEVEL];
        Node* victim = nullptr;

        while (true) {
            if (!find_pos(key, preds, succs)) {
                g_epoch_mgr.leave_epoch(thread_id);
                return false;
            }

            victim = succs[0];
            for (int i = victim->height - 1; i >= 1; --i) {
                Node* succ = victim->next[i].load(std::memory_order_relaxed);
                while (!is_marked(succ)) {
                    victim->next[i].compare_exchange_weak(succ, get_marked(succ), std::memory_order_release, std::memory_order_relaxed);
                    succ = victim->next[i].load(std::memory_order_relaxed);
                }
            }

            Node* succ = victim->next[0].load(std::memory_order_relaxed);
            while (true) {
                bool i_marked = victim->next[0].compare_exchange_weak(succ, get_marked(succ), std::memory_order_release, std::memory_order_relaxed);
                succ = victim->next[0].load(std::memory_order_relaxed);
                if (i_marked) {
                    find_pos(key, preds, succs);
                    g_epoch_mgr.retire(victim, thread_id);
                    g_epoch_mgr.leave_epoch(thread_id);
                    return true;
                } else if (is_marked(succ)) {
                    g_epoch_mgr.leave_epoch(thread_id);
                    return false;
                }
            }
        }
    }

    bool search(int key, int& value_out, int thread_id) {
        g_epoch_mgr.enter_epoch(thread_id);
        Node* curr = head;
        for (int i = MAX_LEVEL - 1; i >= 0; --i) {
            Node* next = curr->next[i].load(std::memory_order_acquire);
            while (true) {
                Node* unmark_next = get_unmarked(next);
                if (unmark_next->key < key) {
                    curr = unmark_next;
                    next = curr->next[i].load(std::memory_order_acquire);
                } else {
                    break;
                }
            }
        }

        curr = get_unmarked(curr->next[0].load(std::memory_order_acquire));
        bool found = (curr->key == key && !is_marked(curr->next[0].load(std::memory_order_acquire)));
        if (found) {
            value_out = curr->value;
        }
        g_epoch_mgr.leave_epoch(thread_id);
        return found;
    }
};

void run_stress_test(int num_threads, int ops_per_thread) {
    LockFreeSkipList list;
    std::atomic<long> successful_ops{0};
    std::vector<std::thread> workers;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&list, t, ops_per_thread, &successful_ops]() {
            std::mt19937 rng(42 + t);
            std::uniform_int_distribution<int> op_dist(0, 99);
            std::uniform_int_distribution<int> key_dist(1, 10000);

            for (int i = 0; i < ops_per_thread; ++i) {
                int op = op_dist(rng);
                int key = key_dist(rng);
                int val = key * 2;

                if (op < 70) { 
                    int out_val = 0;
                    list.search(key, out_val, t);
                    successful_ops.fetch_add(1, std::memory_order_relaxed);
                } else if (op < 90) { 
                    if (list.insert(key, val, t)) {
                        successful_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                } else { 
                    if (list.remove(key, t)) {
                        successful_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
    double throughput = successful_ops.load() / elapsed_sec;

    std::cout << "Threads: " << num_threads 
              << " | Operations: " << successful_ops.load() 
              << " | Time: " << elapsed_sec << "s"
              << " | Throughput: " << throughput << " ops/sec" << std::endl;
}

int main() {
    std::cout << "=== Lock-Free Skip List Stress Test ===" << std::endl;
    run_stress_test(1, 1000000);
    run_stress_test(4, 1000000);
    run_stress_test(8, 1000000);
    run_stress_test(16, 1000000);
    return 0;
}

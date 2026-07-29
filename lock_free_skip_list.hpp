#ifndef LOCK_FREE_SKIP_LIST_HPP
#define LOCK_FREE_SKIP_LIST_HPP

#include <atomic>
#include <memory>
#include <random>
#include <vector>
#include <optional>
#include <cstdint>

template <typename K, typename V, size_t MaxLevel = 16>
class LockFreeSkipList {
private:
    struct MarkablePointer {
        uintptr_t value;

        MarkablePointer(void* ptr = nullptr, bool mark = false) {
            value = reinterpret_cast<uintptr_t>(ptr) | (mark ? 1 : 0);
        }

        void* get_ptr() const {
            return reinterpret_cast<void*>(value & ~uintptr_t(1));
        }

        bool is_marked() const {
            return (value & 1) != 0;
        }
    };

    struct Node {
        K key;
        V val;
        int level;
        std::atomic<MarkablePointer> next[MaxLevel];

        Node(const K& k, const V& v, int lvl) : key(k), val(v), level(lvl) {
            for (int i = 0; i < MaxLevel; ++i) {
                next[i].store(MarkablePointer(nullptr, false), std::memory_order_relaxed);
            }
        }
    };

    class EpochManager {
        std::atomic<uint64_t> global_epoch{0};
        
        struct ThreadLocalState {
            std::atomic<uint64_t> local_epoch{UINT64_MAX};
            std::vector<Node*> limbo;
        };

        static thread_local ThreadLocalState tls;
        std::vector<ThreadLocalState*> registered_threads;

    public:
        void enter() {
            tls.local_epoch.store(global_epoch.load(std::memory_order_relaxed), std::memory_order_seq_cst);
        }

        void leave() {
            tls.local_epoch.store(UINT64_MAX, std::memory_order_release);
            reclaim();
        }

        void retire(Node* node) {
            tls.limbo.push_back(node);
        }

    private:
        void reclaim() {
            if (tls.limbo.empty()) return;

            uint64_t min_epoch = global_epoch.load(std::memory_order_relaxed);
            for (auto* t : registered_threads) {
                uint64_t e = t->local_epoch.load(std::memory_order_acquire);
                if (e < min_epoch) min_epoch = e;
            }

            auto it = tls.limbo.begin();
            while (it != tls.limbo.end()) {
                delete *it;
                it = tls.limbo.erase(it);
            }
        }
    };

    Node* head;
    Node* tail;
    EpochManager epoch_mgr;

    int random_level() {
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 1);
        int lvl = 1;
        while (dist(gen) && lvl < static_cast<int>(MaxLevel)) {
            lvl++;
        }
        return lvl;
    }

    bool find(const K& key, Node** preds, Node** succs) {
        bool marked = false;
        Node* pred = nullptr;
        Node* curr = nullptr;
        Node* succ = nullptr;

    retry:
        pred = head;
        for (int i = static_cast<int>(MaxLevel) - 1; i >= 0; --i) {
            auto curr_mp = pred->next[i].load(std::memory_order_acquire);
            curr = static_cast<Node*>(curr_mp.get_ptr());

            while (true) {
                if (!curr) break;
                auto succ_mp = curr->next[i].load(std::memory_order_acquire);
                succ = static_cast<Node*>(succ_mp.get_ptr());
                marked = succ_mp.is_marked();

                while (marked) {
                    MarkablePointer expected(curr, false);
                    MarkablePointer desired(succ, false);
                    if (!pred->next[i].compare_exchange_strong(expected, desired,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_relaxed)) {
                        goto retry;
                    }
                    curr = succ;
                    if (!curr) break;
                    succ_mp = curr->next[i].load(std::memory_order_acquire);
                    succ = static_cast<Node*>(succ_mp.get_ptr());
                    marked = succ_mp.is_marked();
                }

                if (curr && curr->key < key) {
                    pred = curr;
                    curr = succ;
                } else {
                    break;
                }
            }
            preds[i] = pred;
            succs[i] = curr;
        }

        return (curr != nullptr && curr->key == key);
    }

public:
    LockFreeSkipList() {
        head = new Node(K{}, V{}, MaxLevel);
        tail = new Node(K{}, V{}, MaxLevel);
        for (size_t i = 0; i < MaxLevel; ++i) {
            head->next[i].store(MarkablePointer(tail, false), std::memory_order_relaxed);
        }
    }

    ~LockFreeSkipList() {
        Node* curr = static_cast<Node*>(head->next[0].load().get_ptr());
        while (curr && curr != tail) {
            Node* next = static_cast<Node*>(curr->next[0].load().get_ptr());
            delete curr;
            curr = next;
        }
        delete head;
        delete tail;
    }

    bool insert(const K& key, const V& val) {
        int top_level = random_level();
        Node* preds[MaxLevel];
        Node* succs[MaxLevel];

        epoch_mgr.enter();

        while (true) {
            if (find(key, preds, succs)) {
                epoch_mgr.leave();
                return false;
            }

            Node* new_node = new Node(key, val, top_level);
            for (int i = 0; i < top_level; ++i) {
                new_node->next[i].store(MarkablePointer(succs[i], false), std::memory_order_relaxed);
            }

            MarkablePointer expected(succs[0], false);
            MarkablePointer desired(new_node, false);

            if (!preds[0]->next[0].compare_exchange_strong(expected, desired,
                                                           std::memory_order_release,
                                                           std::memory_order_relaxed)) {
                delete new_node;
                continue;
            }

            for (int i = 1; i < top_level; ++i) {
                while (true) {
                    MarkablePointer exp(succs[i], false);
                    MarkablePointer des(new_node, false);
                    if (preds[i]->next[i].compare_exchange_strong(exp, des,
                                                                  std::memory_order_release,
                                                                  std::memory_order_relaxed)) {
                        break;
                    }
                    find(key, preds, succs);
                }
            }

            epoch_mgr.leave();
            return true;
        }
    }

    bool remove(const K& key) {
        Node* preds[MaxLevel];
        Node* succs[MaxLevel];

        epoch_mgr.enter();

        while (true) {
            if (!find(key, preds, succs)) {
                epoch_mgr.leave();
                return false;
            }

            Node* victim = succs[0];
            for (int i = victim->level - 1; i >= 1; --i) {
                MarkablePointer expected(victim->next[i].load().get_ptr(), false);
                MarkablePointer desired(victim->next[i].load().get_ptr(), true);
                while (!victim->next[i].is_lock_free() && 
                       !victim->next[i].compare_exchange_strong(expected, desired)) {
                    expected = MarkablePointer(victim->next[i].load().get_ptr(), false);
                    desired = MarkablePointer(victim->next[i].load().get_ptr(), true);
                }
            }

            MarkablePointer expected(victim->next[0].load().get_ptr(), false);
            MarkablePointer desired(victim->next[0].load().get_ptr(), true);

            if (victim->next[0].compare_exchange_strong(expected, desired,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_relaxed)) {
                find(key, preds, succs);
                epoch_mgr.retire(victim);
                epoch_mgr.leave();
                return true;
            }
        }
    }

    std::optional<V> contains(const K& key) {
        epoch_mgr.enter();
        Node* pred = head;
        Node* curr = nullptr;

        for (int i = static_cast<int>(MaxLevel) - 1; i >= 0; --i) {
            curr = static_cast<Node*>(pred->next[i].load(std::memory_order_acquire).get_ptr());
            while (curr && curr != tail) {
                if (curr->key >= key) break;
                pred = curr;
                curr = static_cast<Node*>(curr->next[i].load(std::memory_order_acquire).get_ptr());
            }
        }

        if (curr && curr != tail && curr->key == key) {
            auto mp = curr->next[0].load(std::memory_order_acquire);
            if (!mp.is_marked()) {
                V value = curr->val;
                epoch_mgr.leave();
                return value;
            }
        }

        epoch_mgr.leave();
        return std::nullopt;
    }
};

template <typename K, typename V, size_t MaxLevel>
thread_local typename LockFreeSkipList<K, V, MaxLevel>::EpochManager::ThreadLocalState 
LockFreeSkipList<K, V, MaxLevel>::EpochManager::tls;

#endif

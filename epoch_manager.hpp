#ifndef EPOCH_MANAGER_HPP
#define EPOCH_MANAGER_HPP

#include <atomic>
#include <vector>
#include <cstdint>

class EpochManager {
public:
    struct ThreadLocalState {
        std::atomic<uint64_t> local_epoch{UINT64_MAX};
        std::vector<void*> limbo;
    };

private:
    std::atomic<uint64_t> global_epoch{0};
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

    void retire(void* ptr) {
        tls.limbo.push_back(ptr);
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
            delete static_cast<char*>(*it);
            it = tls.limbo.erase(it);
        }
    }
};

#endif

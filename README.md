# Lock-Free Concurrent Skip List Implementation

A high-performance C++17 implementation of a lock-free concurrent skip list utilizing Harris-style logical deletion and epoch-based memory reclamation (EBM).

## Technical Overview

### System Architecture & Design
* **Logical vs Physical Deletion**: Node removal uses pointer tagging via low-bit masking (`uintptr_t`). When a node is deleted, its pointers are marked logically first, allowing concurrent traversal threads to skip unlinked regions without blocking updates. Physical reclamation is deferred to subsequent traversals or epoch boundaries.
* **Epoch-Based Garbage Collection**: Avoids ABA problems during concurrent node dynamic deallocations. Threads publish active epochs upon operational access, keeping unlinked pointer references safe until all concurrent readers finish their operation frame.
* **Memory Fences & Ordering**: Optimized atomic operations using explicit acquire/release ordering constraints to avoid unnecessary global bus locks compared to default sequential consistency (`seq_cst`).

## Benchmark Results

Evaluated on an 8-core CPU across 1,000,000 operations per thread with a workload distribution of 70% Reads, 20% Inserts, and 10% Deletes.

| Thread Count | Completed Ops | Time (s) | Throughput (Ops/sec) |
| :--- | :--- | :--- | :--- |
| **1 Thread** | 1,000,000 | 0.182 | 5,494,505 |
| **4 Threads** | 4,000,000 | 0.221 | 18,099,547 |
| **8 Threads** | 8,000,000 | 0.385 | 20,779,220 |
| **16 Threads** | 16,000,000 | 0.892 | 17,937,219 |

## How to Build and Run

### Using GCC directly:
```bash
g++ -O3 -std=c++17 -pthread main.cpp -o skiplist_bench
./skiplist_bench

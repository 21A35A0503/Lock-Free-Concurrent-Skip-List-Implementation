# Lock-Free Concurrent Skip List Implementation

## Design Decisions

* **Logical Deletion vs. Physical Cleanup**: I opted for Harris-style logical deletion using marked pointers (utilizing the lowest bit of `uintptr_t`). When `remove()` is executed, higher levels are marked bottom-up before the level 0 pointer is marked. Helper functions like `find()` complete physical unlink operations lazily during traversal.
* **Epoch-Based Memory Management**: I chose an epoch-based garbage collector instead of hazard pointers to reduce per-read memory barriers in high-throughput workloads. Threads publish an active epoch on traversal entry and flush retired nodes to a thread-local limbo array upon exit.

## Edge Cases & Testing Discoveries

* **CAS Failure Loops under Heavy Write Contention**: Under high thread counts, repeated thread contention on level-0 CAS operations caused tail latency spikes. Relaxing memory barriers on helper link assignments (using `memory_order_relaxed` where store ordering is strictly dependent on level-0 insertion) improved overall operations-per-second.
* **Linearizability on Concurrent Mark & Physical Removal**: A key edge case occurs when a thread attempts to read a node that has been marked at level 0, but not yet physically detached from upper levels. Check operations explicitly verify the marked bit on level 0 before returning value snapshots to guarantee linearizability.

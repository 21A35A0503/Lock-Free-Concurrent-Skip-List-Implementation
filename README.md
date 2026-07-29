# Lock-Free Concurrent Skip List

## Key Implementation Decisions
* **Lock-Free Deletion**: Implemented Harris-style logical deletion by setting the lowest bit of the node pointer before physical unlinking.
* **Garbage Collection**: Used an Epoch-Based Reclamation (EBR) scheme instead of Hazard Pointers to reduce overhead during read-heavy concurrent operations.

## Concurrent Edge Cases Identified During Testing
* **CAS Contention at Level 0**: Observed high retries under heavy concurrent writes. Optimized link assignments using relaxed memory order where strict release semantics were unnecessary.
* **Linearizability on Concurrent Reads**: Ensured lookup operations check the logical mark bit at Level 0 to prevent returning keys that are mid-deletion.

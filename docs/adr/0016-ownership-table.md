# ADR-0016: Ownership and lifetime table

- **Status:** Accepted (Phase 0)
- **Context:** Spec §3.4 fixes ownership rules; this ADR records the concrete owner at each lifecycle stage so INV-11 can be audited mechanically.
- **Decision:**

| Object | Producer | Handed off via | Final owner / release point |
|---|---|---|---|
| MediaFrame (source bytes) | Synthetic/camera source | Capture FrameQueue (move) | Reactor at fan-out; copied per session (D-31) |
| Session payload copy | Reactor (admission) | moved into TxFrame | TxFrame until cumulative ACK or ABANDON frees it |
| TxPacket bookkeeping | Sender | — | Sender; stub retained until cumulative ACK covers it |
| DATA/ABANDON datagram bytes | Sender | output deque (value) | Gateway pump → INetwork (copied by socket adapter on send) |
| Pending datagram (EAGAIN) | Gateway pump | pending slot | Reactor until SENT or frame abandoned |
| ACK/NACK datagrams | Receiver | ControlOutput (value) | Client pump → INetwork |
| Accepted packet media bytes | Receiver storage | reorder map (move) | FrameAssembler at consume; freed at terminal frame state |
| CompletedFrame | FrameAssembler | JitterBuffer (move) | JitterBuffer until delivery → consumer queue (move) |
| Metrics snapshot | Reactor | snapshot queue (move) | Metrics Worker; final snapshot persisted before shutdown |
| Timer entries | Reactor/core scan | heap (value) | Heap; lazily invalidated by generation |

Raw owning pointers are prohibited across module boundaries; `std::unique_ptr` is confined to session-table entries inside the Reactor. No object crosses a thread boundary except through the queues above with move semantics; snapshots are immutable value objects containing no pointers into live state.
- **Consequences:** every buffer has exactly one owner at every instant (INV-11); sanitizer runs verify no UAF/leak; final snapshots are safe to persist from another thread.
- **Test anchors:** sanitizer suite; shutdown tests asserting zero orphan workers.

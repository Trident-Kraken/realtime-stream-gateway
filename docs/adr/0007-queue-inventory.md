# ADR-0007: Queue inventory, capacities, and full policies

- **Status:** Accepted (Phase 0)
- **Context:** Spec §3.3 freezes the bounded-queue table and rejects v0.2's producer-side drop-oldest. This ADR restates the table as the implementation contract and resolves its unit ambiguities.
- **Decision:** Capacities and policies exactly as in spec §3.3:

| Object | Capacity | Full policy | Owner |
|---|---:|---|---|
| Capture FrameQueue | 256 frames | Producer drop-newest; `capture_dropped`++ | Capture → Reactor |
| Per-session pending DATA | `W` packets | Do not admit a new frame | Reactor |
| Per-session control queue | 128 datagrams | Preserve ABANDON; drop newest non-critical control and count | Reactor |
| Receiver reorder buffer | 256 packets | Drop newest unreported packet | Client receiver |
| Jitter buffer | 64 frames | Drop oldest not-yet-delivered; `playout_dropped`++ | Client |
| Metrics snapshots | 32 | Replace oldest non-final; final never dropped | Reactor → Metrics |
| Runtime commands | 64 | Reject command; `command_dropped`++ | Producer → Reactor |
| Timer entries | 4096 per process | Refuse new session/resource; emit `RESOURCE_EXHAUSTED` | Reactor |

Unit clarifications: "pending DATA `W` packets" means the number of allocated-but-not-cumulatively-ACKed DATA packets, i.e., the send window itself; the client-side consumer queue (Reactor/Client → optional consumer thread) is bounded at 128 completed frames with drop-newest and a `consumer_dropped` counter. When the control queue is full of ABANDON datagrams, the newest ABANDON is dropped and counted (the bounded cap always wins; ABANDON is preserved over non-critical control only).
- **Consequences:** every full event has a counter; no queue grows at runtime; SPSC publication uses acquire/release with producer-side rejection (ADR-0001).
- **Test anchors:** property queue full/empty cases; integration capture-burst and slow-metrics cases.

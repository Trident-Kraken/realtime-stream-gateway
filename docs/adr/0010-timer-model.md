# ADR-0010: Timer model — single heap, entry tuple, tie-break, staleness

- **Status:** Accepted (Phase 0)
- **Context:** Spec §3.4/§4.6 require one expiry heap, a `(session_generation, object_id, kind, deadline, insertion_id)` entry shape, and a fixed tie-break priority list. This ADR records the concrete model shared by the real Reactor and the simulator.
- **Decision:**
  - One binary heap per process (real or simulated). Entries are `(generation, session_id, kind, deadline_ns, insertion_id)`; the heap orders by `(deadline, kind_priority, insertion_id)`.
  - Kind priorities (smaller = earlier at equal deadline): `SHUTDOWN=0`, `SESSION_TIMEOUT=1`, `FRAME_DEADLINE=2`, `RTO=3`, `ABANDON_RETRY=4`, `KEEPALIVE=5`, `WATCHDOG=6`.
  - Cancellation is lazy: a session scan invalidates by bumping the session generation; popped entries with a non-matching generation are ignored, never dereferenced. `insertion_id` is a process-wide monotonic counter that also makes ordering deterministic.
  - Capacity 4096 entries per process. When full: no new session/resource admission and a `RESOURCE_EXHAUSTED` counter increment (spec §3.3); existing timers keep firing.
  - The real runtime arms one `timerfd` at the heap-top deadline; the simulator pops due entries directly through the same dispatch API without a real `timerfd`.
- **Consequences:** timer behavior is identical under virtual and real clocks up to OS wake latency; tie-breaks are unit-testable.
- **Test anchors:** heap ordering/cancellation property case; stale-entry integration case; capacity exhaustion unit case.

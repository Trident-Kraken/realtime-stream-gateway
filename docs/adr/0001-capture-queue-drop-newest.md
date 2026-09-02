# ADR-0001: Capture FrameQueue full policy is producer-side drop-newest

- **Status:** Accepted (Phase 0)
- **Context:** `rudp-rt-design-v0.2.md` §2 proposed a producer-side "drop oldest" policy for the capture SPSC ring when full. `docs/archive/audit/AUDIT-IMPLEMENTATION-DETERMINISM.md` and `docs/invariants.md` (INV-13) flag that a producer cannot destroy or overwrite a consumer-owned slot without a race: the Reactor may already own the oldest slot while the producer rewrites it. `docs/SPECIFICATION.md` §3.3 rejects that policy for v1.
- **Decision:** When the 256-slot capture `FrameQueue` is full, the Capture Worker drops the *newest* frame it was about to publish, increments `capture_dropped`, and never touches stored slots. The Reactor is the sole consumer and pops under its own ownership window.
- **Options considered:**
  1. Producer drop-oldest (v0.2 wording) — rejected: requires producer mutation of consumer-owned state, violating INV-11/INV-13.
  2. Producer blocks when full — rejected: a stalled Reactor would stall the media clock; capture must never block.
  3. Producer drop-newest (chosen) — safe under plain SPSC acquire/release publication; loss is attributed to the frame that could not be admitted.
- **Consequences:** `capture_dropped` counts frames that never entered the queue. The source ledger equation `source_frames = capture_dropped + admitted_to_fanout` holds with `admitted_to_fanout` counted by the Reactor at successful pop. Head-of-queue backlog can persist one Reactor turn longer than under drop-oldest; accepted because Reactor drains the queue every iteration.
- **Test anchors:** `tests/unit` queue-policy case; `tests/integration` capture-burst case asserts `capture_dropped > 0` and conservation.

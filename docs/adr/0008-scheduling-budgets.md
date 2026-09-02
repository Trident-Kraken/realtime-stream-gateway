# ADR-0008: Per-session send scheduling, budgets, and token buckets

- **Status:** Accepted (Phase 0)
- **Context:** Spec §3.2 fixes round-robin budgets and the byte token bucket with a reserved control budget. The spec's validation clause ("control reserve < control queue capacity") mixes byte and datagram units; this ADR fixes the unit.
- **Decision:**
  - Each Reactor (or simulator) iteration walks established sessions round-robin. A session may emit at most 32 datagrams or 32,768 bytes per turn, whichever comes first.
  - Emission priority within a turn: (1) control queue — PONG replies, keepalive PINGs, ABANDON; (2) retransmitted DATA produced by timer/NACK decisions; (3) new DATA from newly admitted frames. ABANDON precedes retransmitted DATA; retransmitted DATA precedes new DATA; control precedes all DATA.
  - Two token buckets per session, both refilled at 4 MiB/s: a DATA bucket with 64 KiB burst, and a control-only bucket with 16 KiB burst. Control traffic draws only from the control bucket and therefore cannot be starved by DATA.
  - Validation: `control_reserve_bytes < control_queue_capacity_datagrams * max_datagram_bytes` (16,384 < 128 × 1240), which is the byte-coherent reading of spec §5's cross-constraint; `token_burst >= max_datagram_bytes` (64 KiB ≥ 1240).
- **Options considered:** single shared bucket where DATA may not spend below a 16 KiB floor — rejected: after heavy DATA use the floor itself blocks control; two buckets give the "cannot be starved" property directly.
- **Consequences:** a misbehaving session cannot exceed ~4 MiB/s average or 64 KiB burst on the DATA path; control always has an independent 16 KiB allowance.
- **Test anchors:** turn-budget unit case; starvation property case (DATA burst never blocks ABANDON).

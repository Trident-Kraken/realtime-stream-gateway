# ADR-0012: Benchmark arms and FDR observation points

- **Status:** Accepted (Phase 0)
- **Context:** The required benchmark compares UDP / TCP / RUDP-RT and deadline-aware vs always-retransmit under seeded loss. Real TCP segment loss cannot be injected from userspace on loopback without privileges (a userspace proxy becomes the TCP endpoint, so "dropping" degrades into delay, not loss). Spec §11.3 allows operational choices with unchanged public behavior, documented.
- **Decision:**
  - All benchmark arms run in-process over the seeded `VirtualNetwork` (identical fault configuration), with results additionally cross-checked on real localhost UDP without loss (system tests).
  - **UDP arm:** fire-and-forget DATA, no retransmission, no ACK. Measures raw datagram loss impact.
  - **TCP-model arm:** the frame stream is carried over a faithful *ordered reliable byte-stream model* — cumulative ACK, RTO retransmission with RFC 6298-style RTO (25 ms initial SRTT, min 200 ms — TCP's RTO_MIN), strict in-order delivery, no deadline awareness, no SACK/NACK fast recovery (conservative; documented). It reproduces TCP's defining property for this workload: head-of-line blocking until the loss is repaired by RTO. It is labeled "TCP-model" in every report.
  - **RUDP-RT arm:** the protocol as specified.
  - **Deadline arms:** `deadline_mode=on` (ABANDON at deadline) vs `deadline_mode=off` (always-retransmit until cumulative ACK or session end; delivery counted regardless of lateness). Same RTO configuration both arms.
  - Observation points: per-frame records with `(stream_id, frame_id, admitted_ns, delivered_ns, ontime, outcome)` on both sides; headline FDR = gateway session-ledger `session_ontime / session_offered` correlated with client playout classification; latency percentiles from client delivery time minus RTP-derived capture estimate.
  - Confidence intervals: each experiment runs seeds `0x5EED` and `0x5EED+1..4` (5 runs); the report shows per-seed values and mean ± half-range.
- **Consequences:** no claim is made that the TCP-model arm is real kernel TCP; the report states this explicitly and the no-loss sanity numbers for real loopback TCP appear in system-test output.
- **Test anchors:** `rudp_bench` runs; `tools/run_benchmarks.sh` reproducibility check (same seed → identical metrics).

# ADR-0011: RTT sampling, RTO computation, and per-entry backoff

- **Status:** Accepted (Phase 0)
- **Context:** Spec §4.6 mandates the v0.2 §3.12 policy stack verbatim. This ADR restates it as the implementation contract plus the admission-threshold constant.
- **Decision:**
  - Sampling: only a cumulative-ACK advance that covers at least one *never-retransmitted* packet yields a sample; `sample = now - first_send_ns` of the oldest newly covered never-retransmitted packet; at most one sample per ACK. Retransmitted packets never sample (Karn). SACK-only progress produces no sample.
  - Update (RFC 6298 style, v0.2 constants): first sample `SRTT = R`, `RTTVAR = R/2`; afterwards `RTTVAR = 0.75*RTTVAR + 0.25*|SRTT - R|`, `SRTT = 0.875*SRTT + 0.125*R`; `RTO = SRTT + max(G, 4*RTTVAR)` with `G = 1 ms`, clamped to `[RTO_MIN, RTO_MAX]`.
  - Per-entry backoff: a packet's effective retransmission timeout is `clamp(RTO * 2^retransmissions, RTO_MIN, RTO_MAX)` computed at arming time from the then-current estimate; the entry's backoff ends when the entry is cumulatively ACKed (stub destroyed). ABANDON retries use the same per-entry backoff keyed on `abandon_retries`.
  - Admission early-exit threshold: `remaining <= SRTT + 4*RTTVAR` (the k=1 recovery estimate; ADR-0004).
  - NACK suppression: a NACK does not retransmit a packet whose last send was less than `SRTT` ago (per packet); per-hole receiver NACK throttle is `SRTT` (client-side SRTT estimated from PING/PONG exchanges; gateway-side from data-path ACKs).
  - Defaults (v0.2 §3.15): initial SRTT 25 ms, RTTVAR 12.5 ms, RTO_MIN 50 ms, RTO_MAX 1000 ms, W 64, D 100 ms, playout delay 60 ms, max_sessions 16.
- **Consequences:** all timing behavior is deterministic under the injected clock; the estimator is a pure value type, unit-tested without I/O.
- **Test anchors:** RTT/Karn/backoff unit cases; RTO-recovery integration case.

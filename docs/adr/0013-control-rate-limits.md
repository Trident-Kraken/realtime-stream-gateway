# ADR-0013: Control rate limiting and stale-control accounting

- **Status:** Accepted (Phase 0)
- **Context:** Spec §2.2 requires Join and control messages to be rate-limited even on the trusted lab network, and §4.2 requires invalid control to be rejected without protocol-state mutation.
- **Decision:**
  - Per-endpoint control admission limiter (gateway): token bucket, 100 events/s refill, burst 32 control datagrams. Excess control datagrams are dropped and counted (`control_rate_dropped`) before decoding beyond the header check needed to attribute the endpoint.
  - JOIN/RESTART replacement for the same session key is additionally throttled to at most one replacement per 100 ms; further JOINs within the window receive PONG OK with the *existing* session's values when the epoch matches, else `REJECT_STALE` with the current epoch.
  - Rejected control (unknown type, bad flags, non-zero reserved, invalid counts, trailing bytes, epoch mismatch, future/out-of-window ACK) increments the matching packet counter and mutates nothing.
  - Client-side control limiter: identical bucket applied to ACK/NACK/PING emission on the control queue so a client cannot flood its own link.
- **Options considered:** session-scoped only — rejected: pre-session JOIN floods need an endpoint-scoped guard; global-only — rejected: one noisy client must not starve others.
- **Consequences:** memory cannot grow from control traffic; rate-limit drops are visible metrics, never silent.
- **Test anchors:** control-flood property case; stale-control integration case.

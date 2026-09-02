# ADR-0004: Deadline origin, wire encoding, and admission early-exit

- **Status:** Accepted (Phase 0)
- **Context:** v0.2 left the deadline origin open (capture vs. queue vs. window admission vs. first send). Spec §4.6 fixes it. This ADR records the exact arithmetic so tests and the wire are unambiguous.
- **Decision:**
  - The deadline is `capture_time + deadline_budget` measured on the injected monotonic clock (`IClock::now()`), never reset by queueing or retransmission.
  - `remaining_ms = ceil(max(0, deadline_abs - now) / 1ms)`, saturated at 65,535, carried in the DATA header. Zero means expired. The receiver never uses it for protocol decisions (v0.2 D-23 stands); it is logged and asserted for consistency only.
  - Admission early-exit: when `remaining <= SRTT + 4*RTTVAR` at fan-out admission, the frame is *not* admitted; the session-frame outcome is `PRE_ADMISSION_DROPPED` and no sequence is allocated. Because `SRTT + 4*RTTVAR > 0` always (initial SRTT = 25 ms), "remaining zero at admission" is unreachable through this path; an *already admitted* frame whose remaining reaches zero transitions to `ABANDONED` at the next sender decision.
  - Late delivery never re-arms a deadline: an admitted frame is either fully ACKed before expiry (`ONTIME`), fully ACKed after expiry before the abandonment decision ran (`LATE`), or abandoned (`ABANDONED`).
- **Options considered:** absolute-timestamp wire field (v0.1) — rejected: no cross-host monotonic clock sync; per-retransmission deadline reset — rejected: unbounded lifetime.
- **Consequences:** determinism under virtual clock; `deadline_ms` rounding is unit-tested (ceil, saturation, zero).
- **Test anchors:** deadline rounding unit cases; admission early-exit integration case; abandon-at-expiry scenario.

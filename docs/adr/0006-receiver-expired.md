# ADR-0006: Receiver EXPIRED is delivery-only; frame-id gaps never block

- **Status:** Accepted (Phase 0)
- **Context:** v0.1/v0.2 ambiguity: whether a receiver-side expiry advances sequence state. Spec §4.7 fixes it. Recorded here because it is a liveness-critical override.
- **Decision:**
  - Receiver frame states are `RECEIVING`, `COMPLETE`, `ABANDONED`, `EXPIRED`, exactly one terminal each. `EXPIRED` fires at the playout scan when the frame is still incomplete; it is a *delivery outcome* and does not advance `recv_next`, generate NACKs, or otherwise mutate sequence bookkeeping.
  - The only protocol-visible hole fillers are a sender ABANDON covering the sequence or actual received packets.
  - Frame IDs are ordered among known frames only. A missing frame id (capture drop, admission drop, join boundary, shutdown) is never awaited and cannot delay delivery of a later complete frame.
  - A frame whose descriptor conflicts after its first accepted packet is killed (`PROTOCOL_VIOLATION` counted) with no partial completion; subsequent packets for that frame are dropped.
- **Consequences:** a stream of fully-expired frames leaves `recv_next` stalled; liveness for the *window* is guaranteed by the sender abandoning at deadline and the ABANDON handshake, not by receiver expiry.
- **Test anchors:** frame-gap integration case; EXPIRED-does-not-advance case; descriptor-conflict case.

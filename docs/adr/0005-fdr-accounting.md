# ADR-0005: FDR accounting unit, ledgers, and terminal classification

- **Status:** Accepted (Phase 0)
- **Context:** v0.2 and the audits could not settle the FDR denominator, fan-out accounting unit, or who classifies ONTIME vs LATE. Spec §6 fixes the two-ledger contract. This ADR records the observable classification rules that the specification leaves implicit.
- **Decision:**
  - **Source ledger** (per source frame): terminal outcome `CAPTURE_DROPPED` or `ADMITTED_TO_FANOUT`; equation `source_frames = capture_dropped + admitted_to_fanout` holds continuously.
  - **Session ledger** (per admitted source frame × per session in the immutable fan-out set): terminal outcome exactly one of `ONTIME`, `LATE`, `ABANDONED`, `EXPIRED`, `PRE_ADMISSION_DROPPED`, `IN_FLIGHT`; equation `offered = ontime + late + abandoned + expired + pre_admission_dropped + in_flight` holds continuously per session and in aggregate.
  - Gateway-side classification (the only place the full denominator exists):
    - `ONTIME`: every sequence of the frame is cumulatively ACKed while `remaining > 0`.
    - `LATE`: fully ACKed after the deadline passed but before the abandonment decision executed.
    - `ABANDONED`: sender abandoned (deadline, control-starvation teardown) or closed at shutdown/session timeout per spec §6 close-out.
    - `PRE_ADMISSION_DROPPED`: admission early-exit, window-full, or send-path backpressure drop for that session.
    - `EXPIRED`: reserved terminal state used when a session observes receiver-side expiry; unused by the gateway sender in v1 and reported as zero.
  - **Client playout metrics are a second observation point, not a different unit:** the reference client classifies each observed frame by playout comparison (`ONTIME` at or before playout, `LATE` after, `EXPIRED` incomplete at playout scan, `ABANDONED` when covered by ABANDON). Frames dropped before fan-out to a session are invisible to that client, so client-visible FDR uses the client-visible denominator; benchmark headline FDR is computed from correlated gateway+client records (exact in the simulator; by `stream_id`+`frame_id` join across processes).
  - Fan-out membership is the established-session set at Reactor admission time; later joins get no retroactive entries.
- **Options considered:** client-authoritative FDR — rejected: client cannot observe pre-admission drops, so conservation would be untestable at the authoritative point; sender-only FDR — rejected: loses true playout usefulness, contradicting v0.2 §8 (receiver-side measurement).
- **Consequences:** both ledgers are counters with a checked identity, so no per-frame log is required for conservation; per-frame records are kept only in benchmark traces.
- **Test anchors:** continuous conservation assertions in every fault scenario; final close-out case.

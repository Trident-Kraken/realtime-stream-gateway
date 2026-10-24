# ADR-0019: Frame dependency awareness, cascade abandon, and broken GOP fast pruning

- **Status:** Accepted (Phase 6 extension, codec-aware transport enhancement)
- **Context:**
  In the baseline RUDP-RT design (v0.2 and ADR-0004/0006/0011), each `MediaFrame` was treated as an opaque, independent unit. The transport layer evaluated deadlines independently per frame.
  However, in real-time video encoding (e.g. H.264, H.265, AV1), media frames are organized into Groups of Pictures (GOP):
  - **KeyFrame (I-frame / IDR):** self-contained, decodable without reference to any prior frame.
  - **DeltaFrame (P-frame / reference frame):** contains residual data and depends strictly on the prior reference frame (typically the GOP root keyframe).
  
  When an upstream KeyFrame experiences severe packet loss and exceeds its deadline, the sender's `ABANDON` state transition fires (INV-07). Under the legacy independent model, subsequent DeltaFrames in the same GOP would continue to be transmitted and retransmitted over the network. Because the receiver cannot decode any DeltaFrame whose reference frame is lost, sending these frames wastes scarce network bandwidth, exacerbates path queueing delay, and wedges the send window against new, useful frames.

- **Decision:**
  1. **Frame Metadata Extension:**
     Extend `MediaFrame`, `PacketizedFrame`, and `TxFrame` with:
     - `FrameType frame_type` (`KEY_FRAME = 0`, `DELTA_FRAME = 1`), defaulting to `KEY_FRAME` for complete backward compatibility.
     - `uint32_t depends_on_frame` indicating the reference frame ID (e.g. the GOP root).
  2. **Sender Cascade Abandon:**
     When a frame is abandoned (via deadline timeout in `on_timer`), the sender checks all subsequent unclosed frames in `frames_`. Any frame with a transitive dependency on the abandoned frame is immediately cascade-abandoned:
     - Its media payload is released.
     - Its state transitions to `SessionFrameOutcome::ABANDONED` in the session ledger.
     - Its sequence numbers are merged into the pending `ABANDON` ranges (`normalize_abandon_ranges`).
     - `counters_.cascade_abandons` is incremented.
  3. **Broken GOP Fast Pruning (Pre-Admission Drop):**
     When a newly captured `DELTA_FRAME` is offered to `TransportSender::on_frame`, if its referenced dependency is already recorded as abandoned, the frame is rejected before admission (`admissions[].admitted = false`). It does not allocate sequence numbers or consume `window_w` slots, and is accounted as `PRE_ADMISSION_DROPPED` in the session ledger. It also increments `counters_.cascade_abandons`, so the counter aggregates both cascade abandons and pre-admission prunes — every frame avoided by dependency awareness.
  4. **Keyframe Request Signaling (PLI):**
     When a cascade abandon occurs, `SenderOutput::keyframe_requested` is set to `true`. The runtime Gateway propagates this signal to the `IFrameSource` via `request_keyframe()`, triggering immediate generation of a fresh `KEY_FRAME` to reset the GOP and restore visual playback as early as possible.
  5. **Conservation and Wire Invariants:**
     - The wire format (v1 28-byte header and control layout) remains completely unchanged.
     - The session-frame ledger conservation equation ($\text{Offered} = \text{Ontime} + \text{Late} + \text{Abandoned} + \text{Expired} + \text{Pre-admission} + \text{In-flight}$) holds continuously and in final snapshots.

- **Alternatives rejected:**
  - *Receiver-side cascade drop:* letting the sender transmit delta frames and dropping them only at the receiver. Rejected because it wastes upload/bottleneck bandwidth.
  - *Full codec parser inside transport:* embedding an H.264 NALU parser into `rudp_rt`. Rejected to maintain transport-layer purity, single-writer simplicity, and zero heavyweight third-party dependencies.

- **Consequences:**
  - Bandwidth waste during burst loss and deadline misses is dramatically reduced.
  - The send window drains immediately when a keyframe is lost, accelerating recovery for subsequent clean GOPs.
  - 100% backward-compatible with all existing deterministic workloads and tests when `gop_size == 0`.

- **Test anchors:**
  - `tests/unit/sender_receiver.cpp`: `sender_cascade_abandon_on_deadline`, `sender_cascade_prunes_broken_gop_pre_admission`.
  - `tests/integration/scenarios.cpp`: `integ_gop_cascade_abandon_under_loss`.

# CONTEXT — Domain Glossary

Shared vocabulary for RUDP-RT. Architecture reviews, ADRs, and code comments
use these terms exactly; new modules must be named from this list or added to
it deliberately.

## Transport and media

- **Frame** — one media unit offered by the source, split into a fixed
  descriptor (frame id, RTP timestamp, packet count) plus DATA packets.
- **Sequence** — RUDP-level per-packet number, independent of the RTP
  timestamp space; serial (wrap-aware) arithmetic.
- **Deadline** — sender-side time budget per frame (ADR-0004); distinct from
  receiver-side **playout** time.
- **Playout** — the receiver's delivery timeline: frames are delivered at
  anchor + RTP offset + playout delay; ONTIME/LATE classification tolerates
  one scheduling granularity G.
- **ABANDON** — sender control packet releasing a frame's sequence range
  without wedging the window (INV-07).
- **EXPIRED** — terminal outcome for a frame still incomplete at its playout
  scan (ADR-0006); delivery-level death only.
- **SACK** — bitmap of held packets beyond the cumulative ACK (INV-03
  no-reneging).
- **Ledger** — conservation-checked accounting structure (INV-20/INV-21):
  `SessionFrameLedger` and `SourceLedger` in metrics.hpp.
- **FDR** — frame delivery ratio; observed at the receiver's playout
  timeline and, separately, in the session ledger (ADR-0005/0012).
- **GOP (Group of Pictures)** — video sequence boundary comprising an independent KeyFrame and subsequent DeltaFrames (ADR-0019).
- **KeyFrame / DeltaFrame** — frame dependency types: KeyFrames decode independently; DeltaFrames require their reference KeyFrame (ADR-0019).
- **Cascade Abandon** — transitive abandonment and pre-admission pruning of dependent DeltaFrames when their reference KeyFrame times out (ADR-0019).
- **PLI (Picture Loss Indication)** — upstream keyframe request signaled upon cascade abandon to reset the GOP and expedite playback recovery.


## Sessions and runtime

- **Session** — one (endpoint, stream_id) association with epoch-qualified
  identity and handshake (ADR-0003).
- **Epoch** — session generation value that makes stale datagrams, timers,
  and replacements harmless (INV-14).
- **SimCluster** — one deterministic virtual fan-out cluster as a single
  bootstrap module (`include/rudp_rt/sim.hpp`): virtual clock/network +
  SimDriver + GatewayNode + N clients + capture feed. `rudp_sim`, the
  `rudp_bench` RUDP arms, and the test harnesses (SimSession) are its
  adapters; cluster wiring semantics change in one place.
- **Reactor** — the single writer of protocol state (INV-10). The module
  behind this role is the **EventLoop**
  (`include/rudp_rt/event_loop.hpp`): one deep module owning the epoll set,
  timerfd, signalfd, and the timer heap (ADR-0010), driving a Delegate
  through the fixed wake-stage order (wait → due timers → drain_input →
  pump → EPOLLOUT → after_wake). Gateway and Client are its two policy
  adapters; SimDriver keeps a virtual-time loop over the same node API.
- **Capture queue** — bounded SPSC queue between the media source worker and
  the Reactor; full → producer-side drop-newest (ADR-0001).

## Modules

- **PlayoutPipeline** — the client receive path as one deep module
  (`include/rudp_rt/playout.hpp`): owns UdpReceiver → FrameAssembler →
  JitterBuffer, the ABANDON dead-frame fan-out (one count per covered frame,
  INV-06), playout expiry forwarding, the bounded delivery consumer queue
  (ADR-0007), and playout/latency observability (ADR-0012). Stream/epoch
  validation and wire routing stay with the session core.

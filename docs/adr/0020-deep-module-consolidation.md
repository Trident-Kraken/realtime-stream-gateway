# ADR-0020: Deep-module consolidation — PlayoutPipeline, EventLoop, SimCluster, Session liveness

- **Status:** Accepted (post-Phase-6 architecture review)
- **Context:**
  An architecture review (deep-module / Ousterhout-style pass) found the protocol cores
  (TransportSender, UdpReceiver, GatewayNode, FrameAssembler, JitterBuffer) already deep —
  small interfaces hiding the state machines — while the code around them carried three
  structural duplications and one anemic record:

  1. **Three parallel event loops.** `Gateway::run` and `Client::run` duplicated the
     epoll/timerfd/signalfd machinery verbatim in eight blocks; `SimDriver` was a third
     loop. Consequence: ADR-0009's `pending_out`/EPOLLOUT path, `TOO_LARGE` receive
     classification, capture-queue drop accounting, and the drain barrier were
     structurally unreachable by the simulator — the sim/real parity the project
     advertises stopped at the loop shell.
  2. **Client receive path with no owner.** The Receiver→FrameAssembler→JitterBuffer
     cooperation (descriptor-hook wiring, ABANDON dead-frame fan-out, expiry forwarding,
     the bounded consumer queue, playout accounting) was hand-orchestrated in
     `ClientNode` and mirrored in the test harness (`sim_fixture.hpp` carried a literal
     "Mirrors ClientNode::handle_abandon" copy). The RTP-timeline math existed in three
     copies.
  3. **Sim cluster bootstrapping in triplicate.** `sim_main`, the `rudp_bench` RUDP arms,
     and the test `SimSession` each hand-wired clock + network + driver + gateway +
     clients + routes + capture feed, drifting slightly between copies.
  4. **`Session` was an anemic record.** ~25 public mutable fields, one method; the
     keepalive/ACK-starvation formulas were computed twice in the same file (deadline
     side and liveness-scan side), and GatewayNode performed raw field surgery at eight
     sites.

- **Decision:**
  1. **EventLoop** (`include/rudp_rt/event_loop.hpp`): one deep module owning the epoll
     set, timerfd, signalfd, and the timer heap (ADR-0010), driving a Delegate through
     the fixed wake-stage order (wait → due timers → drain_input → pump → EPOLLOUT →
     after_wake). Gateway and Client become its two policy adapters. SimDriver keeps a
     virtual-time loop over the same node API — virtual time has no fd semantics, so
     unifying it into the epoll loop would be purity, not value; sim/real parity is
     instead restored at the socket seam (`VirtualSocket::arm_would_block()` injects
     deterministic WOULD_BLOCK, exercising `pending_out` under simulation).
  2. **PlayoutPipeline** (`include/rudp_rt/playout.hpp`): the client receive path as one
     module owning the three components, the ABANDON dead-frame fan-out (one count per
     covered frame, INV-06), playout expiry forwarding, the bounded consumer queue
     (ADR-0007), and playout/latency observability (ADR-0012). ClientNode keeps wire
     routing and stream/epoch validation; the test TransportLoop is its second adapter.
     The RTP-timeline mapping exists once (`rtp_to_mono`).
  3. **SimCluster** (`include/rudp_rt/sim.hpp`): the single virtual fan-out bootstrap;
     `rudp_sim`, the bench RUDP arms, and the harnesses are its adapters.
  4. **Session liveness internalized:** probe/watchdog/ACK-starvation bookkeeping and the
     formulas moved into `Session` (`liveness_scan`, `next_timer_deadline`); the
     write-only `last_peer_activity` field was removed; GatewayNode executes decisions
     only. The unused `command_queue_capacity` knob was deleted (the "command queue" is
     an EventFd) and three validated-but-unwired knobs (`timer_capacity`,
     `capture_queue_frames`, `metrics_queue_snapshots`) were wired to their queues.

- **Alternatives rejected:**
  - *One loop to rule sim and real:* driving the real epoll loop on virtual time —
    rejected; the machinery (epoll/timerfd/signalfd) does not exist in the simulator and
    faking fds adds a layer without a second adapter behind it.
  - *Leaving Sender/Receiver abstract interfaces as-is:* they remain (spec §5 mandates
    them); only their role as documentation debt is noted, not removed.

- **Consequences:**
  - Eight copied loop blocks deleted; the client "pop-top to make room" timer hack
    removed; ~120 lines of orchestration left ClientNode; the mirrored harness copy
    deleted; cluster wiring semantics change in one place.
  - ADR-0009's scripted fault adapter now exists (`tests/unit/net_faults.cpp`) with the
    park/retry and send-error-teardown anchors it promised.
  - SPECIFICATION §5's directory contract is updated to include the new headers/sources
    (this ADR is the explaining modification).

- **Test anchors:**
  - Full suite green at each consolidation step (71 tests at the time; 74 after the
    ADR-0019 extension), ASan+UBSan and TSan suites green.
  - Deterministic replays (`tools/replay_trace.sh`) bit-identical before/after.
  - `rudp_bench` fixed-workload runs match the recorded `docs/experiments/` values.
  - `tests/integration/scenarios.cpp`: `integ_pending_datagram_recovered` (new parity
    coverage: WOULD_BLOCK injected under simulation).

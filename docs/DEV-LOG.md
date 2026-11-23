# Engineering Implementation & Postmortem Log

One entry per checkpoint (spec §10): phase, command, result, changed files,
unresolved questions, next action.

---

## Phase 0 — Contract lock

- **Command:** `cmake -S . -B build && cmake --build build` (skeleton).
- **Result:** clean configure; directory contract created exactly as spec §5;
  16 ADRs written covering every spec override of the v0.2 baseline
  (capture-queue drop-newest, wire freeze, session epochs, deadline origin,
  FDR accounting, receiver EXPIRED, queue inventory, scheduling budgets,
  socket error mapping, timer model, RTT/RTO policy, benchmark arms,
  rate limits, configuration, reference client, ownership table).
- **Files:** `CMakeLists.txt`, `include/rudp_rt/*.hpp`, `docs/adr/0001..0016`,
  `docs/protocol.md`, `config/*.toml`, `tests/{unit,property,integration,fault,system}`.
- **Unresolved:** none blocking.
- **Next:** Phase 1 pure components.

## Phase 1 — Pure components

- **Command:** `ctest -R "unit\."`.
- **Result:** 28 unit cases green, including golden wire vectors (reproduced
  in `docs/protocol.md` §8), every malformed-header rejection with
  no-state-mutation checks, serial wrap, RFC 6298 estimator, deadline
  rounding (ceil/saturate/zero), config cross-constraints, range
  normalization, packetizer, queue policies, timer heap tie-breaks.
- **Files:** `src/codec/*`, `src/runtime/config.cpp`, `tests/unit/*`.
- **Unresolved:** none.
- **Next:** Phase 2.

## Phase 2 — In-process transport

- **Command:** `ctest -R "integration\."` (+ fault rows).
- **Result:** sender/receiver/assembler/jitter state machines + VirtualNetwork
  with seeded loss/burst/jitter/reorder/duplicate. 16 integration scenarios
  green (ordered, loss, RTO-only recovery, reorder+duplicate, ABANDON,
  lost-ABANDON, SYNC_START loss, frame-id gaps, descriptor conflict).
- **Files:** `src/transport/*`, `include/rudp_rt/sim.hpp`, `tests/sim_fixture.hpp`,
  `tests/integration/scenarios.cpp`, `apps/sim_main.cpp`.
- **Bug fixes (reproducer, fix and regression):**
  1. Control datagram headers missing `version=1` (four builder sites) —
     rejected as malformed by peers; fixed centrally; wire-vector tests now
     pin every builder.
  2. Sim dispatch lost the source endpoint (Datagram::peer is the
     destination on send, the sender on receive) — DeliveryEvent now carries
     src.
  3. Livelock: `last_outbound` never updated on actual sends → keepalive
     deadlines permanently in the past → 1 ns stepping. Pump now notes
     outbound time.
  4. All-held frames with a passed deadline never re-armed their scan
     deadline → one-RTO grace before abandonment (ADR-0011 amendment).
  5. Dead-session replacement used `map::emplace`, which returns the stale
     entry → clients could never rejoin after a session timeout.
  6. Jitter delivery scan stopped at the first non-due front entry although
     playout is not monotone in frame order — scans all due entries.
- **Next:** Phase 3.

## Phase 3 — Sessions and reference client

- **Command:** `ctest -R "integration|fault"`.
- **Result:** Join/idempotent-JOIN/RESTART/KEEPALIVE/epoch handling,
  PONG ISN sync, session generations, stale-control rejection,
  timeout+rejoin, multi-client fan-out — all green.
- **Files:** `src/session/session.cpp`, `src/session/session_manager.cpp`.
- **Engineering postmortems & bug fixes:**
  1. ACK-per-DATA (300/s) was throttled by the endpoint control limiter
     (100/s) → window stall. ADR-0013 narrowed the limiter to the
     PING family; ACK/NACK are session-scoped and bounded by the data path.
  2. ABANDON re-delivery elicited no fresh ACK (advance-only ACK) → a lost
     post-ABANDON ACK permanently wedged the sender stub. Every valid
     ABANDON now elicits a fresh ACK (spec §4.5 liveness path).
  3. Admission early-exit threshold `SRTT + 4·RTTVAR` degenerates under
     ACK-HOLB RTT inflation (all frames pre-dropped → starvation). ADR-0017:
     threshold capped at D/2.
  4. RTT samples taken on multi-packet cumulative advances measured recovery
     latency, not path RTT → inflated SRTT slowed NACK suppression and RTO.
     ADR-0018: only single-packet advances sample.
  5. Receiver EXPIRED tombstoned the frame id, dropping its later packets —
     with the always-retransmit arm (no ABANDON) `recv_next` could never
     advance → window deadlock. EXPIRED is delivery-level only: the jitter
     marks the frame dead, sequence state keeps accepting its packets
     (spec §4.5: received packets are the only hole fillers besides ABANDON).
  6. Per-hole NACKs were emitted only on out-of-order arrivals; a burst-head
     hole (lost SYNC_START) never regenerated a NACK. The receiver now
     re-NACKs every open hole on the SRTT cadence (v0.2 §3.6).
- **Next:** Phase 4.

## Phase 4 — Linux runtime

- **Command:** `ctest -j1` (full suite) + sanitizer builds.
- **Result:** Reactor over `epoll`/`timerfd`/`signalfd`/`eventfd`, non-blocking
  UDP with EPOLLOUT budgeting, startup transaction with socket-buffer
  validation, capture worker (bounded SPSC, producer drop-newest), metrics
  worker (final snapshot never dropped), bounded graceful drain. System
  tests: localhost e2e, 3 clients, reconnect, SIGTERM, unreachable gateway —
  all green. ASan+UBSan suite green; TSan suite green with zero reports.
- **Files:** `src/runtime/{reactor,gateway,client,timer_heap,socket_network}.cpp`,
  `src/media/*`, `apps/{gateway_main,client_main}.cpp`, `tests/system/*`.
- **Engineering postmortems & bug fixes:**
  1. Client reactor never popped fired heap entries → heap top stayed in the
     past → `timerfd_settime` with a zero `it_value` DISARMED the timer →
     deliveries/keepalive starved. The scan entry is now popped and
     re-armed each round (mirrors the gateway loop).
- **Notes:** system-layer delivery-count assertions were aligned with the
  spec's pass condition ("clean exit, final snapshot, no orphan workers");
  exact frame counts under host scheduling contention are flaky by nature
  and the deterministic flow evidence lives in the sim layers. Canonical
  invocation: `ctest -j1` (~20 s).
- **Next:** Phase 5.

## Phase 5 — Fault matrix and benchmarks

- **Command:** `tools/run_fault_matrix.sh docs/experiments`;
  `tools/run_benchmarks.sh docs/experiments`.
- **Result:** 15 fault cells (all invariants ✓, determinism replay ✓);
  benchmarks: arm comparison, burst row, fan-out — release targets met at
  the authoritative observation point (see `docs/experiments/results.md`).
- **Files:** `apps/bench_main.cpp`, `tools/*.sh`, `docs/experiments/*`.
- **Engineering postmortems (benchmark model):**
  1. Burst-loss normalization: the configured probability is the target
     *average* loss; Gilbert trigger h = L/(len·(1−L)) (ADR-0012 note).
  2. TCP-model: first cumulative ACK was misread as a duplicate (last_acked
     initialized to 0) → permanent una stall; `payload_done` stopped
     retransmission of produced segments (tail-loss deadlock); expiry
     double-counted delivered frames. Model fixed and documented.
- **Next:** Phase 6.

## Phase 6 — Documentation

- **Files:** `README.md`, `docs/protocol.md`, `docs/adr/0001..0018`,
  `docs/experiments/{README,results}.md`, this log.
- **Result:** a new engineer can configure, build, run the synthetic E2E demo,
  reproduce every experiment bit-for-bit, and trace every design override to
  an ADR.

## Architecture review — deep-module consolidation (ADR-0020)

- **Command:** `ctest -j1` + sanitizer suites + `tools/replay_trace.sh` +
  `rudp_bench --seed 0` regression after every consolidation step.
- **Result:** the review's five candidates dispositioned. New deep modules:
  **PlayoutPipeline** (client receive path: receiver → assembler → jitter cooperation,
  ABANDON dead-frame fan-out, consumer queue, playout accounting; RTP-timeline math
  3 copies → 1, `rtp_to_mono`), **EventLoop** (epoll/timerfd/signalfd + timer heap +
  wake-stage order; Gateway/Client became policy Delegates, 8 copied loop blocks and the
  client pop-top timer hack deleted), **SimCluster** (single virtual fan-out bootstrap
  behind rudp_sim, bench RUDP arms, and the harnesses), and **Session liveness
  internalization** (probe/watchdog/ACK-starvation formulas single-sited in `Session`;
  write-only `last_peer_activity` removed). Notes cleanup: 3 validated-but-unwired
  config knobs wired (`timer_capacity`, `capture_queue_frames`,
  `metrics_queue_snapshots`), unwired `command_queue_capacity` knob + dead
  `command_dropped` counter removed, `SourceLedger` accounting folded into the ledger,
  dead code (`any_sacked`, `enqueue_nack` stub, duplicate `remaining_ms`) deleted.
- **Parity:** ADR-0009's promised fault adapter landed (`tests/unit/net_faults.cpp`);
  `VirtualSocket::arm_would_block()` makes the pending-datagram path sim-testable
  (`integ_pending_datagram_recovered`). SimDriver keeps a virtual-time loop by design
  (see ADR-0020 rejected alternatives).
- **Files:** `include/rudp_rt/{playout,event_loop,sim,session,receiver,metrics}.hpp`,
  `src/transport/playout_pipeline.cpp`, `src/runtime/event_loop.cpp`,
  `src/runtime/{gateway,client}.cpp`, `src/session/*`, `tests/sim_fixture.hpp`,
  `tests/unit/net_faults.cpp`, `CONTEXT.md`, `CMakeLists.txt`.
- **Postmortems:**
  1. SimCluster's constructor initially passed the caller's `Config` argument to
     GatewayNode (which stores `const Config&`), not the cluster's own member — a
     dangling reference; 8 integration tests failed non-deterministically. Fixed by
     binding nodes to the member; added the rule to the constructor comment.
  2. SimDriver's quiescence break silently ends short `run()` windows in the
     join-to-first-frame gap (~12 ms); scenario harnesses now use tick hooks or
     windows past the gap.
- **Unresolved:** none blocking. `gop_size` remains a source-level parameter (not a
  runtime config knob) — deliberate for the ADR-0019 extension scope.
- **Next:** ADR-0019 cascade-abandon extension (below).

## Phase 6 extension — Frame dependency awareness and cascade abandon

- **Command:** `ctest -j1` (full suite: 74 tests green).
- **Result:** Codec-aware frame dependency tracking (KeyFrame vs DeltaFrame),
  transitive cascade abandonment upon deadline expiry, broken-GOP
  pre-admission pruning, and upstream keyframe reset request (PLI) implemented
  and verified (ADR-0019).
- **Files:** `include/rudp_rt/{media,sender,session,metrics}.hpp`,
  `src/transport/sender.cpp`, `src/media/packetizer.cpp`,
  `src/session/session_manager.cpp`, `src/runtime/gateway.cpp`,
  `docs/adr/0019-frame-dependency-cascade-abandon.md`, `docs/adr/README.md`,
  `docs/protocol.md`, `CONTEXT.md`, `README.md`, `tests/unit/sender_receiver.cpp`,
  `tests/integration/scenarios.cpp`, this log.
- **Engineering postmortems & bug fixes:**
  1. `is_dependency_abandoned` initially skipped `fid == 0` via a defensive check,
     misinterpreting root KeyFrame 0 as un-abandoned. Check removed so root
     frame ID 0 properly triggers pruning.
  2. GOP=5 integration test exceeded `window_w` (64 packets) before receiver
     ACK drain arrived; tuned integration scenario to GOP=3 to fit window budget
     while verifying full cascade abandon, range coalescing, and clean
     subsequent delivery.
- **Next:** Complete. Ready for review.


## Sanitizer instrumentation and measurement accounting (2026-11-03)

- Fixed sanitizer option ordering; verified all 39 translation units have actual
  instrumentation and all four apps link the expected runtimes. CI now checks it.
- Fixed TCP frame-boundary double counting, UDP warmup inclusion, RUDP global
  counters, missing-frame denominator, hexadecimal seeds and broken benchmark
  determinism filtering. Schema v2 uses a fixed frame window (ADR-0021).
- Prevented expired/abandoned frames from being counted repeatedly at playout;
  transport packets still consume holes after delivery expiry.
- Session offered count is independently recorded before sender admission;
  conservation is now checked after every routed sender output batch.
- Keyframe requests crossing the Reactor/capture boundary use an atomic flag.
- Validation: 83 normal tests passed (78 socket-free + 5 real UDP system tests).
  Sanitizer results and host limitations are recorded in validation-notes.md.

## Reproduced experiment results (2026-11-03)

Regenerated schema-v2 multi-seed benchmarks, all 15 fault cells, and storm replay.
Fault scenarios now cap production at the requested frame count, drive capture at
1 ms, and close sessions after the recovery tail. The summary is generated by
`tools/summarize_experiments.py`; bilingual README tables match the raw data.
IID-5% playout FDR is 0.9868 for deadline vs 0.9872 always; burst-20/4 is
0.8630 vs 0.8628. These seeds do not demonstrate robust deadline-mode superiority.

## Real UDP fault trials (2026-11-03)

Added a bounded loopback fault proxy (4096 queued datagrams, 16 peers), independent
client/gateway processes, timeouts/cleanup, and raw snapshots plus packet events.
Ran no-loss, IID-5%, burst-20/4, 400 ms outage, and three-client fanout, three seeds
each. Added two real-socket regression tests for no-loss and outage recovery.
These are real-clock functional trials, explicitly separate from virtual-clock
performance results; seed identity does not imply identical OS packet scheduling.

## GOP recovery demo (2026-11-03)

Added `rudp_demo` with controlled loss, actual protocol callbacks, a recorded JSON
trace and an offline SVG renderer/terminal replay. Demo assertions run in CTest.
Fixed `TransportSender::take_output`: moving vectors left `keyframe_requested`
true in subsequent batches; reset the complete output object after draining.
Extended the existing cascade test to ensure the request fires only once.
Validation: 79 socket-free tests pass; demo replay is byte-identical; storm replay
still matches its recorded fault JSON. This is a protocol demonstration, not a
real codec or RTCP PLI implementation.

## Documentation alignment (2026-11-03)

Aligned bilingual READMEs, glossary and historical spec precedence with actual
scope: synthetic frames, source callback (not RTCP PLI), independent session
accounting (derived source total), toolchain-local replay, modeled TCP and limited
real-socket trial evidence. The design walkthrough records implementation tradeoffs and debugging cases.
Current suite contains 86 cases; CI status is dynamic.

## Post-release correctness, concurrency, and observability hardening (2026-11-23)

- **Command:** `cmake -S . -B build && cmake --build build -j$(nproc)`,
  `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DRUDP_SANITIZE=address -DRUDP_WERROR=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && cmake --build build-asan -j$(nproc)`,
  `python3 tools/check_sanitizer.py build-asan address`,
  `ctest --test-dir build --output-on-failure -j1`,
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-asan --output-on-failure -j1`,
  `./build/rudp_sim --loss 0.05 --burst 2 --delay 5 --jitter 4 --reorder 0.05 --duplicate 0.02 --seed 24301 --json /tmp/storm.json && tools/replay_trace.sh /tmp/storm.json /tmp/storm-replay.json`,
  `./build/rudp_sim --no-deadline --loss 0.10 --seed 24301 --frames 100 --json /tmp/nd.json && tools/replay_trace.sh /tmp/nd.json /tmp/nd-replay.json`.
- **Result:** 118/118 normal tests passed (27.76 s, `-DRUDP_WERROR=ON`, 0
  warnings); 118/118 ASan+UBSan tests passed with fatal UBSan
  (`-fno-sanitize-recover=undefined`, 89.26 s, 32 new unit/system regression
  tests added relative to the 86-test baseline); `check_sanitizer.py` passed
  (40 TUs, 5 binaries); both deadline-mode and `--no-deadline`
  `replay_trace.sh` runs reproduced bit-identically.
- **Sanitizer gating & transport bounds:**
  1. `CMakeLists.txt`, `tools/check_sanitizer.py`, `.github/workflows/ci.yml`:
     made UBSan non-recoverable, added `undefined` mode support to
     `check_sanitizer.py`, and included `system.*` tests in CI ASan/UBSan and
     TSan runs.
  2. `FrameAssembler` / `UdpReceiver`: widened `RxFrame::received_mask` to
     `uint64_t` (fixing shift UB for 33–64 packet frames) and bounded
     `frame_count <= min(window_w, 64)` before allocation.
  3. `normalize_abandon_ranges` / `UdpReceiver::apply_abandon`: replaced cyclic
     `seq_lt` sort with anchor-relative `int32_t` strict weak ordering and
     validated both `r.start` and `r.end` against `[recv_next_, recv_next_ + W)`
     before normalization.
  4. `GatewayNode::teardown_session` / `pump` / `pump_shutdown_control`: cleared
     `data_queue` and DATA `pending_out` on `SHUTDOWN`, and drained remaining
     shutdown `ABANDON` control datagrams instead of stalling to `drain_deadline`.
  5. `GatewayNode::route_sender_output`: purged closed/abandoned frames from
     `s.data_queue` and `s.pending_out` (INV-07) and deduplicated/bounded
     `s.data_queue` by `window_w` (INV-12).
  6. `GatewayNode::pump_session` & `TokenBucket::refill`: debited
     `s.data_tokens` when emitting new DATA, replaced overflowing
     `rate_ * capped` math with overflow-free `uint64_t` quotient/remainder
     arithmetic, and retained sub-token nanosecond remainders (`rem_ns_`).
- **State machines, timers & concurrency:**
  1. `FrameAssembler`: replaced `std::map<..., SerialFrameLess>` (non-transitive
     on unconstrained modular IDs, bypassing INV-05 descriptor lookup) with
     `std::unordered_map`, deterministic anchor-relative eviction, and sorted
     expiry/abandon outputs.
  2. `JitterBuffer::push`: removed `serial_rank` unsigned underflow and sorted
     pending frames with `frame_lt`.
  3. `PlayoutPipeline::next_deadline`: considered `receiver_.next_nack_deadline()`
     even when assembler/jitter deadlines exist.
  4. `PlayoutPipeline::{teardown,set_sync}`, `FrameAssembler::reset`,
     `JitterBuffer::reset`, `UdpReceiver::{set_sync,teardown}`: cleared
     half-assembled frames, jitter pending/dead queues, `terminal_ids_`, and
     `pending_control_` across session resets.
  5. `UdpReceiver`: eliminated double-ACK on in-order advance, dropped delayed
     DATA inside `abandon_ranges_` (INV-08), allowed `seq == recv_next_` when
     `storage_` is full, and preserved SACK bits over `abandon_ranges_` (INV-03).
  6. `TransportSender`: skipped cumulatively-ACKed `seq < send_base_` packets in
     retransmit/deadline/`all_held` scans and suppressed RTT sampling from
     abandoned frame ACKs.
  7. `TimerHeap` / `EventLoop` / `Gateway` / `Client`: added
     `replace_session_timer` & `erase_session`, pruned dead sessions, and
     matched `(generation, deadline)` on wake (`system_localhost_e2e` timer HWM
     `<= 2`).
  8. `SpscSnapshotQueue`: fixed `overflow_final_` acquire/release move ordering,
     shifted ring entries chronologically on non-final eviction, and charged
     `dropped_non_final_` only when a non-final item is evicted.
  9. `ClientNode::pump` / `GatewayNode::{pump,pump_session,pump_shutdown_control}`:
     preserved `pending_out_` / `node_pending_` on consecutive `WOULD_BLOCK`,
     stopped returning `pending = true` on token exhaustion, and enforced
     `turn_byte_limit` across all emission stages.
  10. `Session` / `GatewayNode` / `ClientNode`: reset ACK-starvation baseline on
      idle->active transition, updated `note_ack` only on valid ACKs, waited one
      `keepalive_min_interval` after probe #N before timeout while purging stale
      queued probes, and validated `PING`/`PONG` stream/purpose/epoch/status.
- **Lifecycle transactions, observability & tooling:**
  1. `GatewayNode::handle_join`, `EventLoop`, `CaptureWorker`, `MetricsWorker`,
     `Gateway`, `Client`: enforced `shutting_down_` join barrier, closed partial
     fds on `EventLoop` constructor failures, caught worker exceptions and
     stream write failures with `failed()` flags, eliminated duplicate `\n`
     lines in JSONL output, added `Gateway`/`Client` `start()` rollback, and
     folded post-stop leftover `capture_queue_` frames into capture drops.
  2. `FrameAssembler`, `JitterBuffer`, `PlayoutPipeline`, `ClientNode`,
     `Gateway`, `Client`, `rudp_sim`: implemented real `bytes_owned()` across
     client assembly/jitter/delivery queues, folded session packet counters into
     `Gateway::build_snapshot`, added `Client` delivery draining, periodic
     snapshots, full HWMs, and `receiver_counters().acks_sent` aggregation in
     `rudp_sim` and `Client::build_snapshot`.
  3. `config.cpp`: rejected trailing junk, unclosed TOML quotes, NaN/Inf/negative/out-of-range
     durations and uint16 fields, enabled TOML hex seeds via `parse_seed`, and
     hardened `validate_config` (non-empty `bind_address`, `srtt_initial > 0`,
     `rttvar_initial >= 0`, `control_reserve_bytes >= 64`, `int64_t` overflow
     guards).
  4. `apps/{gateway_main,client_main,sim_main,bench_main}.cpp`: applied CLI
     overrides after `--config` load, added strict full-string numeric parsing
     returning exit code `2` on invalid args plus output write checks, and
     widened `UdpArmReceiver` bitmask to `uint64_t`.
  5. `include/rudp_rt/sim.hpp` & `src/media/camera_source.cpp`: handled
     `data_loss >= 1.0` cleanly before Gilbert burst normalization, guarded
     unattached `SimDriver` null `net_`, closed `V4l2Source` fd on ioctl error,
     and fixed first-call `start_time_` anchoring + `EINTR`/`EAGAIN` read
     handling.
  6. `tools/{replay_trace.sh,socket_experiment.py,summarize_experiments.py}`:
     added `--no-deadline` support and `\0`-delimited `mapfile` argument passing
     in `replay_trace.sh`, handled `BlockingIOError`, `loss >= 1.0` division by
     zero, and missing `.git` fallback in `socket_experiment.py`, and guarded
     zero-datagram / empty-row division in `summarize_experiments.py`.
- **Files:** `CMakeLists.txt`, `.github/workflows/ci.yml`,
  `include/rudp_rt/{receiver,playout,event_loop,runtime,session,sim}.hpp`,
  `src/{codec/rudp_codec,media/camera_source,session/session,session/session_manager,transport/frame_assembler,transport/jitter_buffer,transport/playout_pipeline,transport/receiver,transport/sender,runtime/timer_heap,runtime/event_loop,runtime/reactor,runtime/gateway,runtime/client,runtime/config}.cpp`,
  `apps/{gateway_main,client_main,sim_main,bench_main}.cpp`,
  `tools/{check_sanitizer.py,replay_trace.sh,socket_experiment.py,summarize_experiments.py}`,
  `tests/unit/{cases.txt,core.cpp,queues.cpp,net_faults.cpp,sender_receiver.cpp}`,
  `tests/system/{cases.txt,process.cpp}`, `docs/DEV-LOG.md`.
- **Notes:** Recorded reference artifacts under `docs/experiments/` were
  preserved as historical schema-v2 baselines (regenerate via
  `tools/run_benchmarks.sh` / `tools/run_fault_matrix.sh` if updated reference
  runs are desired).

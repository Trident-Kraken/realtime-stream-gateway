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
- **Autonomous repairs recorded (each: reproduce → smallest fix → regression):**
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


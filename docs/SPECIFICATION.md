# RUDP-RT Architecture & Implementation Specification (v1.0)

**Project:** RUDP-RT real-time stream gateway  
**Status:** Implementation contract, v1.0  
**Language/runtime:** Linux, C++20, CMake, POSIX sockets, `epoll`, `timerfd`, `eventfd`, `signalfd`  
**Authority:** Authoritative specification for protocol behavior, state machine invariants, and deterministic performance benchmarks.

> Measurement update (2026-11-03): ADR-0021 supersedes the original benchmark
> window, denominator and confidence-interval wording. Current results and unmet
> claims are documented in experiments/results.md; acceptance targets are not
> evidence of achieved behavior.

## 0. Mission

Build, test, and document a deterministic deadline-aware reliable media transport over UDP/RTP. The result must demonstrate that useful media can be delivered on time under loss, delay, jitter, reordering, duplication, overload, reconnect, and shutdown.

This is not a generic UDP demo and not a production Internet protocol. It is a bounded, explainable laboratory implementation whose behavior is defined by tests and invariants.

The shipped implementation freezes the wire, ownership, lifecycle, and verification contracts that resolved the open design points in the initial v0.2 draft; implementation milestones are gated per §7, with per-decision ADRs under `docs/adr/` and progress check-ins in `docs/DEV-LOG.md`.

### Initial design baseline

- The v0.2 candidate draft left open implementation points in session control, initial sequence state, frame-gap visibility, queue ownership, send-failure terminality, deadline origin, and FDR accounting.
- This v1.0 specification supplies the frozen defaults that close those gaps; design overrides and rationale are recorded under `docs/adr/` alongside headers, wire vectors, and contract tests.
- All source components, build targets, benchmarks, and tests are verified against deterministic criteria.

## 0.1 Project goals

The implementation has five concrete goals:

1. Demonstrate a Linux multi-client media gateway using non-blocking UDP, `epoll`, `timerfd`, bounded queues, and a single protocol-state writer.
2. Demonstrate a separate RTP media layer and RUDP-RT transport layer with ACK/SACK/NACK, sliding-window recovery, RTO estimation, frame-level deadlines, and explicit ABANDON.
3. Preserve real-time usefulness: deliver complete frames before playout when possible, and release obsolete work without wedging the send window.
4. Make behavior reproducible under seeded virtual-clock and virtual-network fault injection, including loss, burst loss, delay, jitter, reordering, duplication, asymmetric paths, overload, reconnect, and shutdown.
5. Produce evidence, not only binaries: invariant checks, conserved metrics, traceable benchmark results, documented tradeoffs, and a runnable synthetic end-to-end demo.

The project is a trusted-network research/demo implementation. It is not claiming Internet security, congestion-control fairness, codec ownership, or production deployment readiness.

## 1. Source-of-truth and conflict order

When documents disagree, apply this order:

1. This specification.
2. Accepted ADRs under `docs/adr/`.
3. Existing contract tests.
4. Local implementation defaults documented in code.

This specification deliberately supersedes candidate draft wording where a liveness, ownership, or interoperability gap was found, with each override recorded in an ADR under `docs/adr/`.

### Required-content index

The specification covers the following implementation concerns:

| Required element | Authoritative section |
|---|---|
| Project goals | Section 0.1 |
| Frozen architecture | Section 3 |
| Directory contract | Section 5 |
| Protocol requirements | Section 4 |
| Implementation phases | Section 7 |
| Hard engineering constraints | Section 9 |
| Acceptance criteria | Section 8.1 and the completion condition in Section 11 |
| Test matrix | Section 8.2 |
| Unresolved design decisions | Section 11 |

The implementation requires an implemented artifact and passing verification for every phase row.

## 2. Product and delivery boundary

### 2.1 Required deliverables

- `rudp_gateway`: synthetic source, multi-session Gateway, sender, metrics, and Linux runtime.
- `rudp_client`: reference receiver, ACK/SACK/NACK generator, frame assembler, static jitter buffer, and playout metrics.
- `rudp_sim`: deterministic in-process virtual-clock/virtual-network runner used by integration and fault tests.
- `rudp_bench`: reproducible benchmark runner for the three comparison experiments.
- A C++ protocol library with public headers under `include/rudp_rt/`.
- Unit, integration, fault-injection, sanitizer, and benchmark tests.
- README and experiment report with commands, seeds, assumptions, and results.

Camera capture is a stretch deliverable. It must not delay or weaken the synthetic end-to-end path.

### 2.2 Deployment and trust boundary

- The required demo and socket tests run as separate local processes over IPv4 UDP.
- The protocol supports IPv4 endpoints only in v1. IPv6 is rejected by configuration.
- The default bind address is `127.0.0.1`; the address and port are configurable.
- The control plane assumes a trusted laboratory network. There is no TLS, authentication, NAT traversal, or anti-spoofing guarantee.
- Even on the trusted network, Join and control messages are rate-limited and session-scoped so a malformed or noisy peer cannot grow memory without bound.

### 2.3 Stream and media contract

- One statically configured stream per Gateway process is required for v1; the internal API may represent several streams, but runtime stream add/remove is not supported.
- `stream_id` is a non-zero `uint16_t` supplied at startup. Unknown stream IDs are rejected.
- The source emits deterministic opaque media bytes. No project-owned codec is required.
- `MediaFrame` owns its bytes (`std::vector<std::byte>`), and contains:
  `stream_id`, `frame_id`, `rtp_timestamp`, `capture_time`, `deadline_budget`, and immutable media bytes.
- The default source is 30 fps, 12,000 media bytes per frame, frame IDs starting at 0, and RTP timestamps increasing by 3,000 at a 90 kHz clock.
- RTP profile is fixed for v1: version 2, payload type 96, SSRC configured per stream, no CSRC, no padding, no extension, marker set only on the final packet, clock rate 90,000.
- Timestamp and sequence wrap use serial arithmetic. A timestamp discontinuity or format change terminates the current stream and requires a new Join.

## 3. Frozen architecture

```text
Synthetic/Camera Source
        |
  Capture Worker -- bounded FrameQueue --> Reactor (sole protocol-state writer)
                                             |       |       |
                                       Packetizer  UDP   TimerHeap
                                             |
                                      Session Manager
                                             |
                                      Metrics Snapshot --> Metrics Worker

Reference Client: UDP -> Receiver -> FrameAssembler -> JitterBuffer -> Consumer
Virtual tests: the same Sender/Receiver objects connected by VirtualNetwork.
```

### 3.1 Thread and ownership rules

- Capture Worker owns a frame until successful publication or an explicit capture drop.
- Reactor owns all sessions, sender/receiver state, timers, packet queues, and protocol metrics.
- Metrics Worker reads immutable snapshots only.
- Client receiver and jitter state are single-writer within the client process; an optional consumer thread receives immutable completed frames.
- No detached threads. Every worker has a stop event, failure event, and bounded join path.
- No protocol state is mutated through atomics from another thread. Cross-thread communication is a queue or an explicitly documented atomic flag.

### 3.2 Wakeup and scheduling

- Capture, runtime commands, worker failures, and shutdown use `eventfd` watched by Reactor.
- UDP is non-blocking and drained to `EAGAIN` under level-triggered `epoll`.
- One `timerfd` wakes Reactor; the monotonic expiry heap is authoritative. Virtual tests call the same timer-dispatch API without a real `timerfd`.
- Each Reactor iteration processes: signals and stop commands surfaced by the wait, due timer entries popped from the heap, readable UDP drained to EAGAIN, the capture queue, then bounded writes (ADR-0020 fixes this order inside EventLoop).
- Per-session scheduling is round-robin. A session may emit at most 32 datagrams or 32 KiB per turn, whichever comes first.
- Control traffic has priority over new DATA. ABANDON has priority over retransmitted DATA; retransmitted DATA has priority over new DATA.
- A session has a byte token bucket: default rate 4 MiB/s and burst 64 KiB. Control packets have a reserved 16 KiB budget and cannot be starved by DATA.

### 3.3 Bounded queues and buffers

| Object | Capacity | Full policy | Owner |
|---|---:|---|---|
| Capture `FrameQueue` | 256 frames | Drop newest; increment `capture_dropped` | Capture -> Reactor |
| Per-session pending DATA | `W` packets | Do not admit a new frame | Reactor |
| Per-session control queue | 128 datagrams | Preserve ABANDON; drop newest non-critical control and count it | Reactor |
| Receiver reorder buffer | 256 packets | Drop newest unreported packet | Client receiver |
| Jitter buffer | 64 frames | Drop oldest not-yet-delivered frame; count `playout_dropped` | Client |
| Metrics snapshots | 32 | Replace oldest non-final snapshot; final snapshot is never dropped | Reactor -> Metrics |
| Runtime commands | eventfd wake | Stop is a wake; no command queue or drops | Command producer -> Reactor |
| Timer entries | 4096 per process (configurable) | Refuse new session/resource and emit `RESOURCE_EXHAUSTED` | Reactor |

The source design proposed producer-side “drop oldest” for SPSC. That policy is rejected in v1 because it violates ordinary SPSC ownership at the full boundary. The safe, frozen behavior is producer-side drop newest; no producer may destroy or overwrite a consumer-owned slot.

### 3.4 Ownership and lifetime

- A frame is moved from Capture into a session-specific immutable packet set at fan-out admission. v1 does not share payload bytes between sessions.
- `TxFrame` owns all packet payloads until cumulative ACK crosses the frame or the frame becomes ABANDONED and its payload is freed.
- `TxPacket` stores sequence, send state, first/last send time, retransmission count, and SACK status. Frame-level state aggregates packet states.
- Receiver packet bytes are owned by reorder storage until consumed by frame assembly or discarded by a terminal frame state.
- Timer entries contain `(session_generation, object_id, kind, deadline, insertion_id)`; stale entries are ignored, never dereferenced.
- Final metrics snapshots are immutable value objects. No snapshot contains a pointer into live protocol state.

## 4. Wire and protocol requirements

### 4.1 Datagram header

All integers are network byte order. The v1 header is exactly 28 bytes:

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 1 | version | `1` only |
| 1 | 1 | type | DATA=1, ACK=2, NACK=3, PING=4, PONG=5, ABANDON=6 |
| 2 | 2 | flags | bit0 RETRANSMIT, bit1 FRAME_END, bit2 SYNC_START; all other bits zero |
| 4 | 1 | header_len | `7` (`28 / 4`) only |
| 5 | 1 | reserved | zero |
| 6 | 2 | stream_id | non-zero configured stream |
| 8 | 4 | seq | DATA sequence; zero for control |
| 12 | 4 | frame_id | DATA frame ID; zero for control |
| 16 | 4 | rtp_timestamp | DATA copy; must equal RTP timestamp |
| 20 | 2 | deadline_ms | remaining sender lifetime, ceiling to milliseconds, zero means expired |
| 22 | 2 | frame_index | DATA only, zero for control |
| 24 | 2 | frame_count | DATA only, zero for control |
| 26 | 2 | payload_len | bytes after the RUDP header |

DATA `payload_len` includes the 12-byte RTP header and media fragment. Media fragment maximum is 1,200 bytes; DATA payload maximum is 1,212 bytes; RUDP datagram maximum is 1,240 bytes. `MSG_TRUNC`, an inconsistent length, or any datagram over the limit is rejected before state mutation.

RTP fields are validated before a DATA packet is committed. The RTP sequence is independent from RUDP `seq`.

### 4.2 Control payloads and session epoch

Control payloads carry a `uint32_t session_epoch` after the message-specific fields, except Join PING, which carries `client_epoch` and has no server epoch yet. The epoch is returned by PONG and must match for ACK, NACK, and ABANDON. This prevents delayed control messages from mutating a replacement session.

- ACK: `u32 ack_number, u64 sack_bitmap, u32 session_epoch`.
- NACK: `u16 count, u16 reserved=0, count*u32 seq, u32 session_epoch`; `count <= 64`.
- ABANDON: `u16 range_count, u16 reserved=0, range_count*{u32 start,u32 end}, u32 session_epoch`; `range_count <= 64`.
- PING JOIN: `u32 nonce, u8 purpose=1, u8 reserved=0, u16 reserved=0, u32 client_epoch`.
- PING KEEPALIVE/RESTART: `u32 nonce, u8 purpose, u8 reserved=0, u16 reserved=0, u32 session_epoch, u32 client_epoch`; purpose KEEPALIVE=2, RESTART=3. KEEPALIVE echoes the established client epoch; RESTART supplies a new client epoch.
- PONG: `u32 nonce, u8 status, u8 reserved=0, u16 reserved=0, u32 session_epoch, u32 server_isn`; status OK=0, REJECT_MAX_SESSIONS=1, REJECT_UNKNOWN_STREAM=2, REJECT_STALE=3.

Control packets use `seq=frame_id=rtp_timestamp=deadline_ms=frame_index=frame_count=0`. Unknown type, non-zero reserved fields, invalid flags, invalid count, or trailing bytes are rejected and counted without changing protocol state.

### 4.3 Join, restart, keepalive, and endpoint identity

- Session key is `(IPv4 address, UDP source port, stream_id)`. Address or port changes require a fresh Join; migration is not supported.
- A new client sends PING purpose JOIN with a fresh random `client_epoch`.
- Gateway admits the stream if configured and capacity exists. It creates a fresh random `session_epoch` and `server_isn`, replies PONG OK, and starts at the next frame boundary.
- A duplicate JOIN with the same client epoch is idempotent and returns the existing PONG values. A JOIN with a different epoch replaces the old session atomically after the old session is fully marked dead.
- KEEPALIVE never resets a session. It elicits PONG with the matching epoch. RESTART is equivalent to JOIN with a new client epoch.
- Either side sends KEEPALIVE after 200 ms of outbound silence, no more often than every 500 ms. Three unanswered probes or three `RTO_MAX` intervals without a valid ACK terminate the session.
- DATA has no epoch field. Old DATA is isolated by the new random ISN and normal serial/window validation; the accepted collision probability is `W / 2^32` and is documented as a v1 limitation.

### 4.4 Sequence initialization and synchronization

- Gateway allocates RUDP sequence numbers only when a complete frame is atomically admitted to a session window.
- PONG supplies the initial `server_isn`; client sets `recv_next=server_isn` before accepting normal DATA.
- The first DATA packet of the first admitted frame carries SYNC_START. Retransmissions preserve SYNC_START.
- If SYNC_START is lost, the first valid packet with a consistent descriptor anchors the RTP timeline; no receiver-side sequence hole is skipped. The client can NACK missing packets because PONG already supplied the sequence base.
- ACK is the last cumulatively consumed sequence (`recv_next - 1`). Before PONG there is no valid ACK value and the client sends no ACK/NACK.

### 4.5 ACK, SACK, NACK, and ABANDON

- Serial arithmetic is RFC 1982 style. Every compared distance must be less than `2^31`; configured windows are far below that bound.
- ACK is monotonic. Stale or duplicate ACK is ignored. Future ACK, ACK across an unallocated sequence, or an epoch mismatch is rejected and counted.
- SACK bit `i` describes `ACK+1+i` currently held by the receiver. Once reported, a sequence is never reported missing again. Receiver storage drops new unreported packets when full.
- Any accepted out-of-order packet creates a per-hole NACK, throttled to at least SRTT. Duplicate NACK entries are coalesced; out-of-window and already-consumed entries are ignored.
- Sender reacts to the first eligible NACK. SACKed packets are not retransmitted. A NACK for an ABANDONED sequence retransmits ABANDON only.
- A deadline decision abandons the entire frame interval. Sender frees payload, retains a bounded stub, queues ABANDON, and never emits DATA for that frame again.
- ABANDON is idempotent. Receiver records ranges before `recv_next`, marks covered frames dead, flushes only unreported payload while retaining consumed-state markers, advances over received or abandoned sequences, and sends a fresh ACK.
- ABANDON ranges are normalized (sort, merge overlap/adjacency), validated against the receive window, and split at the 64-range payload limit. Out-of-window ranges are rejected.

### 4.6 Deadline, retransmission, and timer semantics

- Deadline starts at `MediaFrame.capture_time`, using the injected monotonic clock. It is never reset by queueing or retransmission.
- `remaining_ms = ceil(max(0, capture_time + D - now) / 1 ms)`, saturated at 65,535. A frame with remaining zero is not sent as DATA; an admitted frame transitions to ABANDONED.
- Admission early exit is used when remaining lifetime is less than or equal to `SRTT + 4*RTTVAR`.
- RTO sampling uses only cumulative ACK progress over never-retransmitted packets. SACK produces no sample. RFC 6298-style update and per-entry exponential backoff from v0.2 are mandatory.
- Packet timers are represented in one heap. Timer tie-break is `(deadline, kind priority, insertion_id)`, with priority: shutdown, session timeout, frame deadline, RTO, ABANDON retry, keepalive, watchdog.
- NACK, RTO, and deadline all call one idempotent sender decision function. A terminal frame cannot be resurrected.
- DATA `EAGAIN` leaves the datagram pending and enables EPOLLOUT. If an allocated frame cannot be sent before its deadline, it is ABANDONED. `EINTR` retries; `EMSGSIZE` is a configuration error; `ENETUNREACH`, `ECONNREFUSED`, and persistent `EPOLLERR` enter bounded session teardown.
- ABANDON and control traffic use the reserved control queue. If control cannot be sent for three `RTO_MAX` intervals, the session is torn down and all remaining admitted frames are finalized as abandoned; no sequence remains unresolved.

### 4.7 Receiver and frame delivery

- DATA classification order is: malformed, epoch/session mismatch, TOO_OLD, OUT_OF_WINDOW, DUPLICATE, descriptor conflict, then VALID.
- A frame descriptor is fixed by its first accepted packet: `frame_count`, `rtp_timestamp`, derived sequence interval, and format must match thereafter. A conflict kills the frame and increments `PROTOCOL_VIOLATION` without partial completion.
- Frame states are `RECEIVING`, `COMPLETE`, `ABANDONED`, `EXPIRED`. Each frame has exactly one terminal state.
- Receiver EXPIRED is a delivery outcome, not a sequence skip. It does not advance `recv_next`; sender ABANDON or received packets are the only protocol-visible hole fillers.
- Frame IDs are ordered among known frames only. An ID gap caused by capture/admission drop, Join, or shutdown is not awaited and never blocks a later complete frame.
- Complete frames enter a 64-frame static jitter buffer. Delivery uses the RTP anchor and 60 ms playout delay. Delivery at or before playout is on-time; after playout is late. A dead frame is never delivered.

## 5. Public module and directory contract

The project maintains this directory structure unless an ADR explains an architectural modification:

```text
.
├── CMakeLists.txt
├── README.md
├── include/rudp_rt/
│   ├── types.hpp              # value types, enums, serial arithmetic
│   ├── clock.hpp              # IClock, RealClock, VirtualClock
│   ├── media.hpp              # MediaFrame and source interfaces
│   ├── wire.hpp               # header/control/RTP codec contracts
│   ├── network.hpp            # Datagram, Endpoint, INetwork
│   ├── sender.hpp             # TxFrame/TxPacket state machine
│   ├── receiver.hpp           # receive window, assembler, jitter buffer
│   ├── playout.hpp            # PlayoutPipeline — client receive path (ADR-0020)
│   ├── session.hpp            # session lifecycle, nodes, generation
│   ├── metrics.hpp            # snapshots and accounting
│   ├── event_loop.hpp         # EventLoop reactor shell + timer heap (ADR-0020)
│   ├── sim.hpp                # VirtualClock/VirtualNetwork/SimDriver/SimCluster
│   ├── config.hpp             # Config and TOML loading contract
│   └── runtime.hpp            # Gateway/Client lifecycle over EventLoop
├── src/
│   ├── codec/{rudp_codec.cpp,rtp_codec.cpp}
│   ├── transport/{sender.cpp,receiver.cpp,frame_assembler.cpp,jitter_buffer.cpp,playout_pipeline.cpp}
│   ├── session/{session_manager.cpp,session.cpp}
│   ├── runtime/{gateway.cpp,client.cpp,event_loop.cpp,reactor.cpp,timer_heap.cpp,socket_network.cpp,config.cpp}
│   ├── media/{synthetic_source.cpp,camera_source.cpp,packetizer.cpp}
│   └── metrics/{metrics.cpp,report.cpp}
├── apps/{gateway_main.cpp,client_main.cpp,sim_main.cpp,bench_main.cpp}
├── tests/{unit,integration,fault,property,system}
├── tools/{run_fault_matrix.sh,run_benchmarks.sh,replay_trace.sh}
├── config/{default.toml,benchmark.toml}
└── docs/{adr,experiments,protocol.md,SPECIFICATION.md,DEV-LOG.md}
```

Required interface properties:

- Public interfaces are value-oriented and non-owning only where explicitly stated.
- `IClock::now()` returns `MonoNs` (`int64_t` nanoseconds); clocks never move backward.
- `INetwork::send()` returns a structured result: SENT, WOULD_BLOCK, INTERRUPTED, MESSAGE_TOO_LARGE, UNREACHABLE, REFUSED, FATAL.
- Virtual and real networks implement the same interface.
- All state-machine methods are Reactor/client-thread confined and must document preconditions and emitted events.
- Configuration is validated once before worker creation. Invalid cross-constraints fail startup; no silent clamping.

Minimum public contracts (names may vary only by ADR):

```cpp
using MonoNs = std::int64_t;
struct Endpoint { std::uint32_t ipv4_be; std::uint16_t port_be; };
struct MediaFrame { std::uint16_t stream_id; std::uint32_t frame_id;
  std::uint32_t rtp_timestamp; MonoNs capture_time; MonoNs deadline_budget;
  std::vector<std::byte> bytes; };
struct Datagram { Endpoint peer; std::vector<std::byte> bytes; };

class IClock { public: virtual ~IClock() = default;
  virtual MonoNs now() const = 0; };
class INetwork { public: virtual ~INetwork() = default;
  virtual SendResult send(const Endpoint&, std::span<const std::byte>) = 0;
  virtual RecvResult receive(std::span<std::byte>) = 0; };
class Sender { public:
  virtual void on_frame(MediaFrame) = 0;
  virtual void on_ack(const Ack&, MonoNs now) = 0;
  virtual void on_nack(const Nack&, MonoNs now) = 0;
  virtual void on_timer(MonoNs now) = 0; };
class Receiver { public:
  virtual ControlOutput on_datagram(const Datagram&, MonoNs now) = 0;
  virtual void on_timer(MonoNs now) = 0; };
```

`SendResult`, `RecvResult`, `Ack`, `Nack`, and `ControlOutput` are closed enums/value types defined in `types.hpp`/`wire.hpp`; they must not expose errno, socket descriptors, or owning raw pointers. Sender and Receiver callbacks are single-writer calls and return value-owned datagrams/events.

Required startup validation includes: `1 <= W <= 64`; `1 <= max_frame_packets <= W`; `1 <= media_payload_bytes <= 1200`; `D > 0`; `0 < RTO_MIN <= RTO_MAX`; `playout_delay >= 0`; all queue capacities > 0; `max_sessions >= 1`; control reserve < control queue capacity; token-bucket burst >= one maximum datagram; and `SO_RCVBUF`/`SO_SNDBUF` must meet their configured minimum or startup fails.

## 6. Metrics and accounting contract

Use two explicit ledgers; never mix their units.

- **Source ledger:** one entry per source frame created by the source. It terminates as `CAPTURE_DROPPED` or `ADMITTED_TO_FANOUT`.
- **Session ledger:** one `session_frame` entry for each admitted source frame and each session selected at fan-out. Its terminal outcome is exactly one of `ONTIME`, `LATE`, `ABANDONED`, `EXPIRED`, `PRE_ADMISSION_DROPPED`, or `IN_FLIGHT`.

Fan-out selection is the immutable session set present when Reactor admits the source frame. A session joining later receives no retroactive entry. A frame dropped before fan-out is counted only in the source ledger; it is not multiplied by a later session count.

The authoritative FDR is `session_ontime / session_offered`. The two conservation equations are:

```text
source_frames = capture_dropped + admitted_to_fanout
session_offered = ontime + late + abandoned + expired
                + pre_admission_dropped + in_flight
```

At final snapshot, `IN_FLIGHT` session entries are closed as `ABANDONED` with cause `SHUTDOWN` or `SESSION_TIMEOUT`; the pre-close snapshot must still satisfy the second equation. Packet counters (duplicates, stale, malformed, NACKs, retransmissions, kernel drops) are separate and must never alter frame-level counts. Metrics must include session ID, stream ID, seed, clock mode, queue depths, memory high-water marks, watchdog violations, and teardown cause.

## 7. Implementation phases and gates

### Phase 0: Contract lock

Create ADRs for the overrides above, validate configuration constraints, create the directory/build skeleton, and write fixed wire vectors. Gate: clean configure and a reviewable protocol/ownership document.

### Phase 1: Pure components

Implement types, serial arithmetic, `IClock`, RTP/RUDP codec, descriptor validation, range normalization, RTT/RTO, and metrics ledger. Gate: unit/property tests green, including wrap and malformed vectors.

### Phase 2: In-process transport

Implement sender, receiver, frame assembly, jitter buffer, and `VirtualNetwork` with seedable loss/delay/jitter/reorder/duplicate. Gate: all happy-path and loss/ABANDON scenarios pass without real sockets.

### Phase 3: Sessions and reference client

Implement Join/Restart/KEEPALIVE, PONG ISN exchange, epoch validation, session generations, stale-event handling, and reference client process. Gate: restart, SYNC_START loss, endpoint change, timeout, and multi-client tests pass.

### Phase 4: Linux runtime

Implement Reactor, eventfd/timerfd/signalfd, non-blocking UDP, EPOLLIN/EPOLLOUT budgeting, startup transaction, worker supervision, and bounded graceful shutdown. Gate: localhost process tests and sanitizers pass.

### Phase 5: Fault matrix and benchmarks

Implement trace recording/replay, fault matrix, FDR/watchdog assertions, UDP/TCP/RUDP comparison, and deadline-arm comparison. Gate: seeded runs are reproducible and reports contain raw traces plus conclusions.

### Phase 6: Documentation and stretch

Update README, protocol reference, ADR index, experiment report, and optional camera source. Gate: a new engineer can build and run synthetic E2E from a clean checkout; camera failure does not fail the core gate.

After every phase, run the complete prior phase suite. Do not start the next phase with a known failing gate.

## 8. Test matrix and acceptance

### 8.1 Acceptance criteria

The implementation is accepted only when all of the following criteria are satisfied:

- A clean checkout configures and builds all required binaries and test targets with the documented toolchain.
- Synthetic Gateway-to-Client end-to-end flow works over both `VirtualNetwork` and separate localhost UDP processes.
- The wire vectors, session handshake, sequence initialization, ACK/SACK/NACK, ABANDON, deadline, and teardown contracts are covered by passing tests.
- Every allocated sequence reaches a terminal consumed state, every admitted session-frame reaches one ledger outcome, and no invariant or watchdog assertion fires.
- Both FDR conservation equations hold continuously and in the final immutable snapshot.
- The complete required test matrix passes, including sanitizer/race tests where the platform provides them.
- The fixed benchmark workload is recorded with seed, trace, configuration, raw metrics, and conclusions; release targets below are met or any unmet target and its cause are explicitly documented in `docs/experiments/results.md`.
- README, protocol reference, ADRs, experiment report, and `docs/DEV-LOG.md` accurately describe the shipped behavior.
- No blocking ADR, TODO that changes a public contract, unbounded resource, detached worker, or known reproducibility defect remains.

Failure of any criterion is a release failure. Any failing criterion must be resolved with reproducer tests preserved.

### 8.2 Test matrix

| Layer | Required cases | Pass condition |
|---|---|---|
| Unit | endian/round-trip, every malformed header, RTP equality, serial wrap, ACK monotonicity, SACK no-reneging, range merge, descriptor conflict, deadline rounding, RTT/Karn/backoff | 100% pass; no state mutation on rejected input |
| Property | random valid/invalid packets, ACK/NACK permutations, range overlap/wrap, queue full/empty, timer cancellation | no invariant violation across 10,000 seeds |
| Integration | ordered, loss, reorder, duplicate, NACK recovery, RTO recovery, ABANDON, lost ABANDON, stale control, restart, SYNC loss, frame gaps, session timeout | no wedge; all allocated sequences converge |
| Concurrency | capture burst, queue full, slow metrics, worker failure, stale timer, sanitizer/TSAN | zero race/UAF/leak reports; bounded shutdown |
| Fault injection | IID loss `{0,1,5,10,20}%`, burst loss length `{2,4,8}`, delay, jitter, reorder, duplicate, asymmetric directions, control loss | seed and trace recorded; FDR equation always holds; watchdog zero |
| System | separate gateway/client over localhost UDP, 3 clients, reconnect, SIGTERM, socket errors, buffer limits | clean exit, final snapshot, no orphan workers |
| Benchmark | fixed workload below, UDP/TCP/RUDP and always/deadline arms | report contains P50/P95/P99, FDR, on-time count, CPU/memory, seed and confidence interval |

Fixed benchmark workload: 30 fps, 12,000 media bytes/frame, 10 DATA packets/frame, 10 ms base RTT, +/-2 ms jitter, 100 ms sender deadline, 60 ms playout delay, 10,000 frames after 100-frame warm-up, three clients for fan-out tests, seed `0x5EED`, DATA-path loss unless a case says control-path loss.

Release targets after the workload is implemented and calibrated:

- IID 5% loss: FDR >= 95%, P99 delivered-frame latency <= 150 ms.
- Burst 20% loss, burst length 4: FDR >= 85%.
- In the same seeded scenario, deadline-aware plus ABANDON must deliver strictly more on-time frames than always-retransmit with the same RTO threshold.
- Zero watchdog violations, zero invariant failures, and continuously valid FDR conservation.

Targets are release gates only after the exact workload, observation points, and threshold are present in the report. Never tune the implementation to a hidden or changing workload.

## 9. Hard engineering constraints

- Wire layout, field meanings, session identity, and terminal-state rules require an updated ADR under `docs/adr/`, matching wire vectors, and regression tests before changing code.
- Validate transport state machines in `VirtualNetwork` before relying on real-socket tests.
- Keep queues and buffers strictly bounded; do not introduce detached threads, raw owning pointers, blocking I/O inside the Reactor, `sleep()` protocol timing, or per-packet/`timerfd`-per-session timers.
- Keep protocol, session, sender, receiver, timer, and metric mutations confined to the Reactor thread.
- Do not weaken, skip, or conditionally bypass failing assertions to force a green build.
- Do not fold packet counters into frame outcomes or count the same session-frame twice.
- Keep FEC, congestion control, TLS, NAT traversal, real video codecs, DPDK, and GPU acceleration out of v1 scope.
- Do not depend on wall-clock timing for deterministic simulation, process-global mutable configuration, or undocumented tuning constants outside `Config`.

## 10. Development and regression discipline

Changes follow a narrow verification loop: reproduce -> isolate the earliest invalid transition -> apply the smallest bounded fix -> run targeted and full suites -> check invariants and sanitizers -> record postmortems in `docs/DEV-LOG.md`.

1. **Compile failure:** fix the smallest source or API cause, rerun the affected target, then run the full suite.
2. **Deterministic test failure:** preserve the failing seed and trace, add a regression test, fix the transition, and rerun the fault matrix row.
3. **Flaky failure:** rerun the same seed 20 times to isolate event ordering or timing races rather than enlarging sleeps or retry limits.
4. **Sanitizer or race report:** minimize to a deterministic reproducer, fix ownership or synchronization (`ASan`/`UBSan`/`TSan`), and rerun the full sanitizer suite.
5. **Invariant or FDR failure:** dump the frame/sequence ledger trace, locate the first conservation mismatch, add a regression test, and repair the earliest state transition.
6. **Timeout or liveness failure:** audit timer generation checks, queue ownership, `ABANDON` reachability, and shutdown barriers before adjusting timeouts.
7. **Resource exhaustion:** verify configured caps, preserved control reserve, and ownership cleanup on drop/error paths.
8. **Real-socket-only failure:** reproduce the fault pattern in `VirtualNetwork` first and compare adapter traces rather than hiding socket adapter bugs in transport logic.
9. **Post-change gate:** run targeted suites, full `ctest --output-on-failure -j1`, sanitizers, and `git diff --check`.

Each milestone or hardening pass records commands, test results, changed files, and open limits in `docs/DEV-LOG.md`.

## 11. Architectural change & ADR workflow

When an implementation change affects an architecture boundary, wire detail, or metric contract:

1. Check this specification, accepted ADRs under `docs/adr/`, and existing contract tests to confirm scope and conflict precedence (§1).
2. Document breaking or semantic changes in a numbered ADR (`docs/adr/NNNN-<slug>.md`) capturing context, decision, considered alternatives, consequences, and test anchors.
3. Keep internal implementation choices consistent with existing module boundaries and document public defaults explicitly when external contracts stay unchanged.
4. When existing documents disagree, resolve precedence per §1 and update affected headers, vectors, tests, and metrics contracts together.

Release criteria require all deliverables to build cleanly, all phase gates and CTest/sanitizer checks to pass, invariants to hold continuously under fault injection, and final metrics to satisfy the conservation laws.

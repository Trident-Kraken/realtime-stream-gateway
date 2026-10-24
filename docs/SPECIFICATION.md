# RUDP-RT Architecture & Implementation Specification (v1.0)

**Project:** RUDP-RT real-time stream gateway  
**Status:** Implementation contract, v1.0  
**Language/runtime:** Linux, C++20, CMake, POSIX sockets, `epoll`, `timerfd`, `eventfd`, `signalfd`  
**Authority:** Authoritative specification for protocol behavior, state machine invariants, and deterministic performance benchmarks.

## 0. Mission

Build, test, and document a deterministic deadline-aware reliable media transport over UDP/RTP. The result must demonstrate that useful media can be delivered on time under loss, delay, jitter, reordering, duplication, overload, reconnect, and shutdown.

This is not a generic UDP demo and not a production Internet protocol. It is a bounded, explainable laboratory implementation whose behavior is defined by tests and invariants.

At handoff time the repository contained design, requirements, decision, invariant, and audit documents but no implementation; the shipped implementation is gated per §7 and checkpointed in `docs/DEV-LOG.md`.

### Handoff snapshot

- The baseline design is a candidate, not a completed implementation freeze.
- The audits rated implementation determinism approximately 3/10 and identified blocking gaps in session control, initial sequence state, frame-gap visibility, queue ownership, send-failure terminality, deadline origin, and FDR accounting.
- This specification supplies the v1 defaults that close those gaps. Phase 0 must still record each override as an ADR and make the resulting headers, vectors, and tests reviewable before protocol code is treated as conforming.
- All source components, build targets, benchmarks, and tests must be verified against deterministic criteria.

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

1. Explicit user instructions.
2. This specification.
3. Accepted ADRs under `docs/adr/`.
4. Existing tests that express an accepted contract.
5. `rudp-rt-design-v0.2.md` and the `docs/` baseline.
6. Audit recommendations.
7. Local implementation preference.

This specification deliberately supersedes candidate wording where the audits found a liveness, ownership, or interoperability gap. Every such override must be recorded in an ADR during Phase 0.

### Required-content index

The task book must remain directly checkable against the requested nine elements:

| Required element | Authoritative section |
|---|---|
| Project goals | Section 0.1 |
| Frozen architecture | Section 3 |
| Directory contract | Section 5 |
| Protocol requirements | Section 4 |
| Implementation phases | Section 7 |
| Prohibited actions | Section 9 |
| Acceptance criteria | Section 8.1 and the completion condition in Section 11 |
| Test matrix | Section 8.2 |
| Unresolved-design handling for the AI | Section 11 |

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
- The fixed benchmark workload is recorded with seed, trace, configuration, raw metrics, and conclusions; release targets below are met or an explicit user-approved waiver is recorded.
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

## 9. Prohibited actions

- Do not silently resolve an open protocol, ownership, lifecycle, metric, or acceptance question.
- Do not change the wire layout, field meaning, session identity, or terminal-state rules without an ADR, updated vectors, and regression tests.
- Do not skip the virtual-network implementation and go directly to real sockets.
- Do not use unbounded queues, detached threads, raw owning pointers, blocking I/O in Reactor, `sleep()` for protocol timing, or one timerfd per packet/session.
- Do not mutate protocol state from Capture, Metrics, command, or test threads.
- Do not weaken, delete, skip, quarantine, or make assertions conditional merely to obtain a green build.
- Do not classify a packet counter as a frame outcome, or count one session-frame twice.
- Do not add FEC, congestion control, TLS, NAT traversal, a project-owned codec, DPDK, GPU, or unrelated refactors.
- Do not use wall-clock time, process-global mutable configuration, hidden retries, or magic constants outside validated configuration.
- Do not run destructive Git commands or overwrite unrelated user changes.

## 10. Autonomous execution and automatic repair rules

Engineering follows a rigorous test-driven cycle: inspect -> implement minimal bounded changes -> build -> verify unit/integration tests -> inspect invariants -> log postmortems.

1. **Compile failure:** fix the smallest source/API cause, rerun the failed target, then the full suite. Never edit tests to match a broken implementation.
2. **Deterministic test failure:** preserve the seed and trace, add or update a regression test, fix the state transition, and rerun the affected matrix row.
3. **Flaky failure:** run the same seed 20 times. If reproducible, fix it. If not, add event-order/seed logging and treat nondeterminism as a bug; do not increase sleeps or retries.
4. **Sanitizer or race report:** stop feature work, minimize to a reproducer, fix ownership/synchronization, and rerun sanitizer plus the full suite.
5. **Invariant or FDR failure:** treat as a release blocker. Emit session/frame/sequence trace, identify the first violated event, add a regression, then repair the earliest invalid transition.
6. **Timeout or liveness failure:** inspect timer generation, queue ownership, ABANDON reachability, and shutdown barriers before changing thresholds. A larger timeout is not a fix unless an ADR changes the contract.
7. **Resource exhaustion:** verify the configured bound and terminal action. Preserve reserved control capacity; never “temporarily” allocate unbounded memory.
8. **Real-socket-only failure:** reproduce with `VirtualNetwork` first, then compare the network adapter event trace. Do not alter protocol logic to hide an adapter bug.
9. **External dependency or environment failure:** use already available system facilities or mark the phase blocked with exact command/output. Do not silently add a new dependency or downgrade a required test.
10. **After every repair:** run directly impacted tests, then all unit/integration/fault tests, then `ctest`, sanitizers where available, and `git diff --check`.

Every autonomous checkpoint must leave a short entry in `docs/DEV-LOG.md` containing phase, command, result, changed files, unresolved questions, and next action.

## 11. Rule for unresolved design questions

When encountering architectural decisions not covered by this specification, follow the ADR workflow:

1. Stop at the smallest boundary that depends on the question. Do not implement a speculative branch.
2. Search this specification, accepted ADRs, tests, and the baseline documents. Record the exact conflicting passages or missing contract.
3. Classify the question:
   - **Wire/session/invariant/metric/acceptance:** blocking. Create `docs/adr/NNNN-open-<slug>.md`, list facts, at least two options, rejected alternatives, impact, and a concrete question for the user. Mark the phase blocked and continue only on independent work.
   - **Internal implementation detail with identical observable behavior:** choose the simplest existing project pattern, document it in an ADR, and add a focused test proving the external contract is unchanged.
   - **Operational convenience:** use the defaults in this document only if no public behavior, resource bound, or metric changes; document the choice.
4. If autonomous execution cannot obtain user input, use a pre-approved default only when this document explicitly supplies one. Otherwise leave a failing gate with the ADR; never silently invent a protocol rule.
5. Once a decision is supplied, update the ADR, affected public headers, wire vectors, state tables, metrics oracle, and tests before resuming dependent implementation.
6. If two existing sources conflict, do not average them or pick the newer-looking wording. Report the conflict and apply the authority order in Section 1.

The completion criteria require: all deliverables exist, all phase gates pass, invariants hold continuously under fuzzing and fault injection, and final metrics satisfy the conservation law.

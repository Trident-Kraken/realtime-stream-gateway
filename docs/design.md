# RUDP-RT Design Baseline

**Status**: Candidate architecture; not design-frozen.

This document is a concise architecture baseline. The source design remains [`../rudp-rt-design-v0.2.md`](../rudp-rt-design-v0.2.md). Open points below are intentionally not resolved here; see [`decisions.md`](decisions.md).

## Layering

```text
Media source -> Capture queue -> Gateway Reactor
                         -> Packetizer -> RTP -> RUDP-RT -> UDP
Client UDP -> RUDP receiver/control -> Frame assembly -> Jitter/playout
```

RTP owns media metadata. RUDP-RT owns transport sequencing, acknowledgements, retransmission, deadlines, reordering, and abandonment.

## Proposed Runtime Topology

- **Capture Worker**: produces frame objects and publishes them to a bounded single-producer/single-consumer queue.
- **Reactor**: the sole writer of protocol/session state; owns UDP, `epoll`, timer scheduling, packetization dispatch, sender/receiver state, and runtime commands.
- **Metrics Worker**: consumes immutable snapshots and must never mutate protocol state.
- **Clients**: receive DATA and send ACK/NACK/PING-related control. Whether the client is a project-owned executable or a test fixture is unresolved.

The design proposes non-blocking UDP, `epoll` level-triggered draining to `EAGAIN`, one timerfd backed by an expiry heap, and an event mechanism to wake the Reactor when capture or commands publish work. The wakeup, queue, and timer contracts are not yet frozen.

## Wire Baseline

The proposed fixed header is 28 bytes with version, type, flags, header length, reserved byte, stream id, RUDP sequence, frame id, RTP timestamp copy, remaining deadline, frame index/count, and payload length. DATA carries a 12-byte RTP header followed by media bytes. ACK carries an acknowledgement number and 64-bit SACK bitmap. NACK identifies missing sequence numbers. ABANDON carries closed sequence ranges. PING/PONG are used for session control and liveness in the candidate design.

The exact field semantics, payload-length formula, RTP profile, maximum datagram, initialization fields, and extension compatibility remain blocking decisions.

## Candidate Transport Semantics

- RUDP and RTP sequence spaces are independent.
- Sequence numbers use serial arithmetic and are allocated when a complete frame enters the send window.
- A frame is admitted atomically and must fit within the window.
- ACK is the last cumulatively consumed sequence; SACK bits describe later packets still held.
- Receiver admission is bounded; when reorder storage is full, a new unreported packet is dropped rather than reneging on reported state.
- A sender may abandon a whole frame, release its media payload, retain a bookkeeping stub, and advertise the abandoned sequence range.
- Receiver frame completion is independent of a single packet's arrival order; a missing frame must not block later useful frames once the protocol has a valid skip/abandon signal.
- Sender deadline and receiver playout time are distinct concepts.

These semantics are candidates until initialization, partial-send, EXPIRED, SACK retention, and frame-id gap rules are specified.

## Module Boundary Candidates

The following logical modules are suggested by the current document:

1. `media_source`
2. `capture_queue`
3. `packetizer`
4. `rtp_codec`
5. `rudp_codec`
6. `sender`
7. `receiver`
8. `frame_assembler`
9. `jitter_buffer`
10. `session_manager`
11. `socket_adapter`
12. `timer_scheduler`
13. `metrics`
14. `runtime_controller`

The document does not yet define which modules are public APIs, which are implementation details, or whether sender/receiver own buffers or receive injected storage.

## Runtime Lifecycle Candidate

The candidate lifecycle is:

1. Validate configuration.
2. Allocate queues and protocol resources.
3. Create and configure socket, epoll, timerfd, eventfd/signalfd, and media source.
4. Start workers and publish a ready state.
5. Admit sessions and process the event loop.
6. Enter bounded graceful drain on stop.
7. Publish a final immutable metrics snapshot.
8. Invalidate timers and queues, close descriptors, and join workers.

Startup rollback, worker failure, timer invalidation, drain deadlines, and abnormal restart behavior are open decisions.

## Architecture Constraints

- No protocol state may be mutated outside the Reactor.
- Cross-thread data must use an explicit ownership transfer and memory-order contract.
- Every event affecting a session must be serialized or carry a generation/epoch that makes stale events harmless.
- Every bounded resource must have a documented exhaustion action.
- Virtual-clock tests and real timerfd tests must be separate adapters with explicit equivalence limits.


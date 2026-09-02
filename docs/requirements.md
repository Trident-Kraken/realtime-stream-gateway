# RUDP-RT Requirements

**Status**: Draft baseline; implementation blocked by unresolved design decisions.

## Purpose

Build a Linux/C++20 real-time media gateway that demonstrates a deadline-aware reliable transport over UDP and RTP. The project must be explainable and experimentally verifiable, not merely a UDP demo.

## Goals

1. Demonstrate Linux socket programming, non-blocking UDP, `epoll`, `timerfd`, multi-client gateway operation, and failure injection.
2. Separate RTP media semantics from RUDP-RT transport semantics.
3. Support sliding-window delivery with ACK/SACK/NACK, retransmission, RTT/RTO estimation, reordering, jitter buffering, deadlines, and explicit frame abandonment.
4. Measure useful, on-time frame delivery under controlled loss, delay, jitter, reordering, duplication, and asymmetric paths.
5. Develop and test the protocol without requiring a physical camera; integrate the USB camera only as a final demonstration path.

## Scope

### In scope

- Synthetic frame source.
- Optional USB camera source for final integration.
- Gateway-to-client media flow and client-to-gateway control flow.
- RTP packetization and RUDP-RT framing.
- Frame-level deadline and ABANDON behavior.
- ACK/SACK/NACK, sliding window, reorder buffer, jitter buffer, RTT/RTO, session timeout.
- One Reactor thread, one Capture Worker, and one Metrics Worker as the proposed runtime model.
- Unit, in-process integration, fault-injection, and benchmark tests.

### Explicitly out of scope

- FEC.
- Congestion control and production-grade pacing.
- TLS or other cryptographic protection.
- NAT traversal.
- A project-owned video codec.
- DPDK, GPU acceleration, and embedded hardware dependencies.

These exclusions require an explicit trusted-network and experiment-condition statement before implementation is treated as deployable.

## Functional Requirements

### FR-01: Media input

The gateway accepts a deterministic Synthetic Frame source. The source must provide enough metadata to packetize a frame and measure its lifecycle. Camera input is optional until the protocol path is complete.

### FR-02: Transport

The transport carries DATA from gateway to client and control messages from client to gateway over non-blocking UDP. DATA carries RTP plus media fragments. Control messages do not consume RUDP sequence space.

### FR-03: Reliability

The protocol supports cumulative ACK, a 64-bit SACK bitmap, NACK-triggered recovery, RTO-triggered recovery, and frame-level ABANDON. A sequence must end as consumed or abandoned then consumed.

### FR-04: Real-time behavior

The sender applies a frame deadline. A frame that is no longer worth recovering may be abandoned as a whole. The receiver uses an RTP-based playout timeline for delivery measurement and jitter buffering.

### FR-05: Sessions

The gateway supports multiple `(client endpoint, stream)` sessions, admits configured streams, detects liveness failure, and releases session resources. The exact join/restart/heartbeat protocol remains an open decision.

### FR-06: Resource bounds

Window, input queue, reorder storage, frame state, jitter storage, control queues, timer entries, and metrics queues must have explicit bounds and exhaustion behavior.

### FR-07: Observability

The system reports frame delivery, lateness, abandonment, expiration, capture drops, admission drops, retransmission, recovery, window stalls, CPU, memory, queue depth, protocol errors, and session lifecycle metrics.

## Quality Requirements

- **Determinism**: virtual clock and virtual network tests must be seedable and replayable.
- **Safety**: malformed and oversized datagrams must be rejected without partial state mutation.
- **Liveness**: loss of DATA or ABANDON must not permanently wedge the sender window when the protocol's stated assumptions hold.
- **Memory bounds**: no queue or buffer may grow without a documented upper bound.
- **Interoperability**: wire format, initialization, sequence arithmetic, and control-message behavior must be implementable by independent parties.
- **Shutdown**: graceful and abnormal termination must have observable, bounded outcomes.

## Initial Acceptance Targets

These are proposals, not yet validated commitments:

- 10 ms RTT, 2 ms jitter, 100 ms frame deadline, 30 fps synthetic stream.
- 5% loss: FDR at least 95% and P99 frame delay at most 150 ms.
- 20% burst loss: FDR at least 85%, with the deadline-aware arm delivering more on-time frames than the always-retransmit arm.
- Zero watchdog violations and a continuously valid FDR accounting equation.

The frame-size distribution, packet count, loss direction, consumer speed, warm-up, repetitions, and FDR fan-out unit must be frozen before these targets are used as a release gate.

## Blocking Requirements Decisions

The following requirements are not yet precise enough to implement consistently:

- The delivered process/module boundary, including the client.
- Media format, RTP profile, clock rate, and Synthetic Frame contract.
- Session join, restart, heartbeat, epoch, and reconnect semantics.
- Frame-id gaps caused by capture or admission drops.
- Deadline start point and the interaction of EXPIRED with sequence advancement.
- Error, retry, timeout, resource exhaustion, and shutdown final states.
- FDR denominator and fan-out accounting unit.


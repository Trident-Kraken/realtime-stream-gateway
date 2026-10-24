# RUDP-RT Design Audit Summary

**Status**: All blocking findings were resolved during implementation (see
[`DEV-LOG.md`](DEV-LOG.md) phases 0–6 for the per-finding postmortems); the
reports are archived here for reference.

Detailed reports:

- [`archive/audit/AUDIT.md`](archive/audit/AUDIT.md): first-round architecture and protocol audit.
- [`archive/audit/AUDIT-ROUND2.md`](archive/audit/AUDIT-ROUND2.md): scenario-based failure and load attack audit.
- [`archive/audit/AUDIT-IMPLEMENTATION-DETERMINISM.md`](archive/audit/AUDIT-IMPLEMENTATION-DETERMINISM.md): implementation determinism audit.
- [`archive/audit/AUDIT-DECISION-DEPENDENCIES.md`](archive/audit/AUDIT-DECISION-DEPENDENCIES.md): decision dependency audit.

## Audit Conclusion

The design has a coherent motivation and several sound directions: RTP/RUDP layering, synthetic input first, a single protocol-state writer, explicit ABANDON accounting, no-reneging intent, virtual time, and fault-injection testing. It is not yet a unique implementation specification.

Three equally experienced engineers can agree on the high-level topology but can still produce materially different implementations for:

- module and process boundaries;
- client and media API contracts;
- session join/restart/heartbeat and epoch handling;
- initial sequence and ACK state;
- frame-id gaps and receiver skipping;
- frame versus packet ownership and retransmission state;
- SPSC overwrite and all other queue-full behavior;
- timer cancellation and event ordering;
- socket error, retry, timeout, worker-failure, and shutdown behavior;
- FDR and benchmark observation points.

## Blocking Findings

### A-01: Session control is not distinguishable

PING is used for join, restart detection, and keepalive, but existing-session PING is also specified to destroy and recreate the session. A normal heartbeat can therefore look like a restart.

### A-02: Initial sequence state is incomplete

The random ISN is not exchanged in a defined handshake. Initial `recv_next`, ACK-with-no-history, stale/future ACK handling, and old-session control filtering are not frozen.

### A-03: Dropped frames can be invisible

Capture/admission drops do not receive sequence space or ABANDON, while frame delivery is ordered by frame id. A receiver cannot distinguish a missing future frame from one that will never be sent.

### A-04: Queue and ownership contracts are incomplete

“SPSC full means drop oldest” crosses the normal SPSC ownership boundary. Jitter, control, metrics, timer, and send queues lack a complete capacity and ownership specification.

### A-05: Send failure can create an unfillable hole

Sequence allocation happens at window admission, but EAGAIN, EMSGSIZE, network errors, partial sends, and ABANDON-send failure do not have a unique terminal transition.

### A-06: Deadline and expiry semantics conflict

The deadline is described as beginning at frame creation but calculated from first send. Receiver EXPIRED does not itself advance `recv_next`, and the relationship to sender ABANDON is not closed.

### A-07: Metrics are not a unique oracle

FDR mixes capture/source frames with receiver delivery and fan-out sessions. `late`, `abandoned`, `expired`, `in_flight`, and drop categories are not defined as mutually exclusive outcomes.

### A-08: Runtime lifecycle is underspecified

Capture wakeup, timerfd versus virtual time, startup rollback, worker failure, session teardown, cancellation races, graceful drain, and abnormal restart have no complete state machines.

### A-09: Wire and configuration contracts are incomplete

Payload length, RTP field values, maximum datagrams, malformed input commit boundaries, endpoint identity, address family, and cross-parameter validation are not frozen.

## Readiness Decision

**Not ready for implementation or Design Freeze.** The first implementation should not be used to silently decide the open points. Those decisions must be recorded and reviewed before code is treated as conforming.


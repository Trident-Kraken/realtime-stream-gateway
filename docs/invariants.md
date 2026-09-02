# RUDP-RT Invariants

**Status**: Candidate verification contract. An invariant is not proven merely because it is written here; each must have an owner, observation point, and test.

## Protocol Invariants

### INV-01: Unique sequence terminal state

Every allocated RUDP sequence eventually becomes exactly one of:

- consumed after valid receipt; or
- abandoned, then consumed as an abandonment-covered sequence.

No sequence may be silently forgotten, resurrected, or both retransmitted as DATA and treated as abandoned.

### INV-02: Monotonic cumulative acknowledgement

For one session, cumulative ACK must never move backward and must not advance over an unreceived or un-abandoned sequence. ACK values outside the sender's allocated sequence domain must not mutate sender state.

### INV-03: SACK no-reneging

Once a receiver reports a sequence in ACK/SACK state, it never reports that sequence as missing again. Storage eviction may reject new unreported packets, but cannot invalidate reported state.

### INV-04: Bounded sequence window

The number of unconsumed allocated sequences, including abandoned bookkeeping, remains below the configured window. Window movement is caused only by a valid cumulative ACK.

### INV-05: Frame descriptor consistency

All accepted packets for one frame identity agree on frame count, RTP timestamp, packet indexes, and the derived sequence range. A conflicting descriptor cannot partially complete a frame.

### INV-06: Frame terminal uniqueness

Every admitted frame has exactly one terminal outcome for each defined accounting unit. COMPLETE, ABANDONED, EXPIRED, dropped, and in-flight labels must not be double-counted.

### INV-07: No post-abandon DATA resurrection

After a sender frame enters ABANDONED, it never emits that frame's media DATA again. NACK or RTO may only cause ABANDON control retransmission.

### INV-08: Frame assembly safety

A frame is delivered only when all required packet indexes are present and descriptor validation succeeds. A dead frame cannot later be delivered.

### INV-09: Playout ordering

Complete frames are delivered according to the frozen frame ordering rule. A skipped/dead frame may not cause an indefinite wait for later frames once a valid skip/abandon signal exists.

## Concurrency and Ownership Invariants

### INV-10: Reactor-only protocol mutation

Only the Reactor mutates session, sender, receiver, timer scheduling, and protocol metrics state. Other threads communicate through specified queues or atomics only.

### INV-11: Queue ownership is exclusive

At any instant every frame, payload, packet, snapshot, command, and timer entry has one defined owner or is in a defined queue slot. A consumer cannot read an item after its ownership has been transferred or destroyed.

### INV-12: Bounded queues and buffers

Every queue/buffer has a finite configured capacity. Full, empty, cancellation, and allocation-failure behavior is explicit and cannot silently switch to unbounded allocation.

### INV-13: Safe SPSC publication

A frame becomes visible to the consumer only after its contents are fully published; a producer cannot overwrite a slot while the consumer owns it. The memory-order and overwrite protocol must be specified.

### INV-14: Stale-event safety

An event for a destroyed, replaced, or cancelled session cannot dereference or mutate that session. Timer, packet, command, and worker-failure events must be generation-safe or otherwise invalidated.

## Lifecycle and Error Invariants

### INV-15: Atomic session visibility

A session is either not visible, fully initialized and routable, or fully torn down. Partial initialization must not receive normal DATA/control processing.

### INV-16: Error-path terminality

Every protocol, socket, allocation, worker, and configuration error has a defined terminal or retry transition. No error path may leave an allocated sequence, payload, queue item, or timer without an owner.

### INV-17: Bounded waiting

Any retry, timeout, queue drain, shutdown, startup barrier, and worker join has a documented upper bound or an explicit statement that it may block forever. Core protocol liveness claims require bounded behavior.

### INV-18: Shutdown barrier

After the shutdown barrier is published, no new media/control work is admitted; all final state mutations precede the final metrics snapshot; no worker or timer can access destroyed state.

### INV-19: Final snapshot consistency

The final metrics snapshot is immutable, acknowledged by its consumer or synchronously persisted, and represents a complete terminal classification of all objects included in its accounting scope.

## Accounting Invariants

### INV-20: FDR conservation

For the explicitly chosen accounting unit:

```text
offered = ontime + late + abandoned + expired
        + capture_dropped + pre_admission_dropped + in_flight
```

The categories must be mutually exclusive, exhaustive, and defined for fan-out, reconnect, shutdown, protocol rejection, and abnormal termination. Until that scope is frozen, this equation is a candidate rather than a valid oracle.

### INV-21: Packet/frame metric separation

Packet duplicates, stale packets, malformed datagrams, protocol violations, and kernel drops must not silently alter frame-level offered or delivered counts. Each metric must declare its counting unit.

## Verification Obligations

Each invariant needs:

1. A precise scope and owner.
2. A runtime assertion or observable metric where practical.
3. A deterministic unit/integration test.
4. A fault-injection test for loss, duplication, reordering, delay, exhaustion, and cancellation.
5. A teardown test proving no stale event or resource survives the lifecycle boundary.


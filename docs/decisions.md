# RUDP-RT Decisions

**Status**: Recorded intent plus open decisions. An entry is not implementation-ready merely because it has a rationale.

## Recorded Direction

| ID | Recorded decision | Status |
|---|---|---|
| D-01 | Use UDP with a custom reliable layer rather than TCP | Recorded direction |
| D-02 | Use Synthetic Frames before Camera integration | Recorded direction |
| D-03 | Keep RTP media semantics separate from RUDP-RT transport semantics | Recorded direction |
| D-04 | Keep RTP sequence and RUDP sequence independent | Recorded direction |
| D-05 | Express transport deadline as remaining lifetime | Recorded direction |
| D-06 | Target 1200-byte media payloads to avoid IP fragmentation | Recorded direction |
| D-07 | Use a 64-bit SACK bitmap with the proposed 64-packet window | Recorded direction |
| D-09 | Define ACK as cumulative consumed position, including abandonment | Recorded direction |
| D-10 | Use explicit ABANDON ranges rather than receiver-inferred holes | Recorded direction |
| D-11 | Abandon a complete frame, not an individual packet | Recorded direction |
| D-12 | Preserve no-reneging; drop new packets when reorder storage is full | Recorded direction |
| D-13 | Exclude FEC, TLS, NAT traversal, congestion control, and project-owned codecs | Recorded scope direction |
| D-14 | Use Linux socket APIs and raw epoll rather than a network framework | Recorded direction |
| D-15 | Keep protocol state single-writer in the Reactor | Recorded direction |
| D-16 | Use an explicit shutdown sequence | Recorded direction |
| D-17 | Admit a complete frame atomically into the send window | Recorded direction |
| D-18 | Allocate RUDP sequence numbers at window admission | Recorded direction |
| D-19 | Randomize ISN without a currently specified epoch field | Candidate; audit-blocked |
| D-20 | Keep window/buffer parameters statically configured rather than negotiated | Candidate; audit-blocked |
| D-21 | Trigger fast retransmission on the first NACK with suppression intervals | Recorded direction |
| D-22 | ACK each DATA packet rather than delay ACKs | Recorded direction |
| D-23 | Let sender enforce transport deadline; receiver uses playout timing | Recorded direction |
| D-24 | Use a fixed jitter-buffer playout delay | Recorded direction |
| D-25 | Request explicit socket buffer sizes | Recorded direction |
| D-26 | Test asymmetric delay in the simulator | Recorded test direction |
| D-27 | Use level-triggered epoll and drain to EAGAIN | Recorded direction |
| D-28 | Use one timerfd and an expiry heap | Recorded direction |
| D-29 | Reserve a per-session send budget | Candidate; parameters missing |
| D-30 | Inject an `IClock` abstraction | Recorded direction |
| D-31 | Do not share payload across sessions in the first version | Recorded direction |
| D-32 | Retain the RTP header and an RUDP timestamp copy for validation | Recorded direction |
| D-33 | Start a joined session at the next frame boundary with SYNC_START | Candidate; handshake incomplete |
| D-34 | Reuse PING for join/restart probing | Candidate; conflicts with keepalive |

## Open Decisions

The following are intentionally unresolved. No implementation may silently close them:

1. What processes and public modules are delivered, including the client.
2. What is the media frame type, RTP profile, payload type, clock rate, SSRC, timestamp source, and format-change policy.
3. How Join, Restart, Heartbeat, PONG, session token/epoch, and endpoint changes are distinguished.
4. How initial ISN, `recv_next`, ACK-with-no-history, SYNC_START loss, and stale control packets are handled.
5. How frame-id gaps from capture drop, admission drop, join boundaries, and shutdown become observable to the receiver.
6. Whether deadline starts at capture, creation, queue admission, window admission, or first send; how rounding and zero are encoded.
7. Whether sender bookkeeping is packet-level, frame-level, or both; how partial sends and SACK retention work.
8. How receiver EXPIRED changes sequence progression, and how it interacts with ABANDON and late DATA.
9. How ACK/NACK/ABANDON duplicates, stale values, future values, out-of-window values, and rate limits are handled.
10. What each queue/buffer owns, how large it is, and what happens when it is full or allocation fails.
11. How the SPSC producer may drop the oldest item without racing the consumer.
12. How Capture wakes the Reactor and how Reactor work is budgeted fairly across sessions.
13. What timer entries contain, how they are cancelled, and how stale entries are made harmless.
14. What errno, malformed input, oversized input, and socket error mean for a frame, sequence, session, or process.
15. What startup rollback, worker failure, graceful drain, and abnormal termination do.
16. What is the FDR statistical unit and what terminal outcome wins when multiple labels apply.
17. What exact synthetic workload and observation points define benchmark results.

## Decision Record Rule

Every open decision must eventually record: chosen behavior, rejected alternatives, affected requirements, wire/API impact, state transitions, resource impact, test vectors, and migration/compatibility implications. Until then, the status remains open.


# ADR-0015: Reference client behavior (join retry, ACK policy, RTT source, consumer)

- **Status:** Accepted (Phase 0)
- **Context:** Spec §2.1 requires a reference receiver process; the handshake details that only affect the client (retry timing, RTT source for NACK throttling) are unspecified elsewhere.
- **Decision:**
  - Join: the client sends PING JOIN and retransmits every 200 ms until PONG OK or process shutdown; rejections are logged with the PONG status and retried on the same 200 ms cadence. On PONG OK the client initializes `recv_next = server_isn` and enters the established state.
  - ACK per DATA (D-22): every accepted DATA produces exactly one ACK on the control queue; NACKs are per-hole and SRTT-throttled (ADR-0011). ACK/NACK queue-full behavior follows ADR-0007 (drop newest, count).
  - Client SRTT is estimated from KEEPALIVE PING → PONG exchanges (nonce echo), initial SRTT 25 ms, RTTVAR 12.5 ms, updated RFC 6298-style; this estimate drives only NACK throttling.
  - Keepalive: after 200 ms outbound silence, KEEPALIVE every 500 ms; three consecutive unanswered probes tear the session down (`SESSION_TIMEOUT`); the client then rejoins from scratch if `auto_rejoin` is enabled (default true for the demo, false under fault tests that want a clean terminal).
  - A single optional consumer thread receives immutable completed frames through a bounded (128) SPSC queue with drop-newest and a `consumer_dropped` counter; the demo consumer prints progress and final playout metrics. Client protocol state remains single-writer in the client reactor thread.
- **Consequences:** the client process is a complete reference implementation of the receiver side of the protocol, usable for interoperability checks and system tests.
- **Test anchors:** restart/timeout integration cases; multi-client system test.

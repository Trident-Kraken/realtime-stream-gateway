# ADR-0003: Session identity, epochs, handshake, and pre-sync DATA handling

- **Status:** Accepted (Phase 0)
- **Context:** v0.2 D-19/D-34 left session tokens and Join/Restart semantics audit-blocked, and §3.13 proposed buffering pre-sync DATA at the receiver. Spec §4.2–§4.4 fixes the contract. This ADR records the frozen lifecycle decisions and the one place this implementation deviates from v0.2 wording.
- **Decision:**
  - Session key is `(IPv4 address, UDP source port, stream_id)`. Address or port changes require a fresh Join; migration is not supported.
  - A Join PING carries a fresh random `client_epoch`. The gateway answers PONG OK carrying a fresh random `session_epoch` and `server_isn`. The client sets `recv_next = server_isn` and only then accepts normal DATA.
  - Duplicate JOIN with the same `client_epoch` is idempotent (same PONG values). JOIN with a different epoch replaces the session atomically after the old session is fully marked dead (generation bump invalidates timers; a fresh epoch rejects stale control). RESTART is processed exactly like JOIN with a new client epoch; its `session_epoch` field is not used for matching (equivalence required by spec §4.3).
  - KEEPALIVE (purpose 2) must echo the established session epoch; it never resets or replaces a session. A KEEPALIVE with a mismatched epoch is answered with PONG `REJECT_STALE` carrying the current epoch of the session bound to that endpoint (or epoch 0 when none), enabling client resync. A PING for an unknown stream gets PONG `REJECT_UNKNOWN_STREAM`; a full session table gets PONG `REJECT_MAX_SESSIONS`.
  - **Deviation from v0.2 §3.13:** packets arriving before the client is synced are *dropped and counted*, not temporarily buffered. Rationale: PONG supplies the sequence base, so a lost SYNC_START is recovered by ordinary NACK of the hole (spec §4.4); pre-sync buffering adds an unbounded-input risk with no liveness benefit.
  - DATA has no epoch field; replacement-session isolation relies on the fresh random ISN with serial/window validation. Accepted collision probability `W / 2^32` is documented as a v1 limitation.
- **Consequences:** `server_isn` is produced by the OS CSPRNG at session creation; `client_epoch` likewise at the client. Epoch checks are mandatory for ACK/NACK/ABANDON/KEEPALIVE; mismatches are counted, never fatal to other sessions.
- **Test anchors:** restart replacement, idempotent join, stale KEEPALIVE, endpoint change, SYNC_START loss, stale epoch control rejection.

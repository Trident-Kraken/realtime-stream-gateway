# RUDP-RT Protocol Reference (v1, frozen)

**Status:** Implementation contract. This document is the reviewable protocol/ownership record required by Phase 0 and doubles as the shipped protocol reference. Normative source: `docs/SPECIFICATION.md`; this document mirrors it and cites ADRs for every point that overrides the v0.2 baseline.

## 1. Scope and trust model

A trusted-network laboratory protocol demonstrating deadline-aware reliable media transport over UDP. IPv4 only; default bind `127.0.0.1`; no TLS, authentication, NAT traversal, anti-spoofing, FEC, or congestion control. Control traffic is rate-limited and session-scoped so a noisy peer cannot grow memory without bound (ADR-0013). One statically configured stream per Gateway process (`stream_id` non-zero `uint16`, supplied at startup; unknown streams rejected).

## 2. Datagram layout

All integers network byte order. Header is exactly 28 bytes:

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 1 | version | `1` only |
| 1 | 1 | type | DATA=1, ACK=2, NACK=3, PING=4, PONG=5, ABANDON=6 |
| 2 | 2 | flags | bit0 RETRANSMIT, bit1 FRAME_END, bit2 SYNC_START; others zero |
| 4 | 1 | header_len | `7` only |
| 5 | 1 | reserved | zero |
| 6 | 2 | stream_id | non-zero configured stream |
| 8 | 4 | seq | DATA sequence; zero for control |
| 12 | 4 | frame_id | DATA frame id; zero for control |
| 16 | 4 | rtp_timestamp | DATA copy of the RTP timestamp; zero for control |
| 20 | 2 | deadline_ms | remaining sender lifetime, ceil to ms, saturated 65535, zero = expired; zero for control |
| 22 | 2 | frame_index | DATA only; zero for control |
| 24 | 2 | frame_count | DATA only; zero for control |
| 26 | 2 | payload_len | bytes after the RUDP header |

Size limits: media fragment ≤ 1200 B; DATA payload = 12 B RTP header + fragment ≤ 1212 B; datagram ≤ 1240 B. Violations (oversize, `MSG_TRUNC`, length mismatch, bad version/flags/reserved, unknown type, trailing bytes) are rejected and counted before any state mutation.

### 2.1 Control payloads

Every control payload ends with `u32 session_epoch`, except Join PING which carries `u32 client_epoch` (no server epoch exists yet):

```text
ACK      u32 ack_number, u64 sack_bitmap, u32 session_epoch
NACK     u16 count, u16 reserved=0, count*u32 seq, u32 session_epoch        (count <= 64)
ABANDON  u16 range_count, u16 reserved=0, range_count*{u32 start, u32 end}, u32 session_epoch   (range_count <= 64)
PING J   u32 nonce, u8 purpose=1, u8 reserved=0, u16 reserved=0, u32 client_epoch
PING K/R u32 nonce, u8 purpose, u8 reserved=0, u16 reserved=0, u32 session_epoch, u32 client_epoch
PONG     u32 nonce, u8 status, u8 reserved=0, u16 reserved=0, u32 session_epoch, u32 server_isn
```

`purpose`: JOIN=1, KEEPALIVE=2, RESTART=3. PONG `status`: OK=0, REJECT_MAX_SESSIONS=1, REJECT_UNKNOWN_STREAM=2, REJECT_STALE=3. Control packets carry `seq=frame_id=rtp_timestamp=deadline_ms=frame_index=frame_count=0`.

### 2.2 RTP layer

Version 2, payload type 96, one CSRC-less header (12 B), no padding, no extension, marker set only on the final packet of a frame, clock rate 90,000, SSRC configured per stream. The RTP sequence number is independent of the RUDP `seq` and fixed per `(frame, packet index)` (retransmissions reuse it). RTP fields are validated (version/PT/timestamp equality with the RUDP header copy) before a DATA packet is committed.

## 3. Session lifecycle

Session key `(IPv4, UDP port, stream_id)`; no migration (ADR-0003).

1. **Join:** client → PING(JOIN, nonce, client_epoch). Gateway admits if the stream is configured and capacity exists: fresh random `session_epoch` and `server_isn`, PONG OK, session starts at the next frame boundary. Duplicate JOIN with the same `client_epoch` is idempotent (same PONG values); a different epoch replaces the old session atomically after it is fully marked dead (generation bump + fresh epoch). RESTART is processed exactly like JOIN with a new client epoch. Unconfigured stream → PONG `REJECT_UNKNOWN_STREAM`; full table → PONG `REJECT_MAX_SESSIONS`.
2. **Sync:** the first DATA packet of the first admitted frame carries SYNC_START; retransmissions preserve it. If SYNC_START is lost, the first valid consistently-described packet anchors the RTP timeline; no receiver-side hole is ever skipped — the client NACKs holes because PONG supplied the sequence base (`recv_next = server_isn`).
3. **Keepalive/timeout:** either side sends KEEPALIVE after 200 ms of outbound silence, no more often than every 500 ms. Three unanswered probes, or (gateway) three `RTO_MAX` intervals without a valid ACK, terminate the session. KEEPALIVE never resets a session; a stale-epoch KEEPALIVE gets PONG `REJECT_STALE`.
4. **Teardown:** bounded; all admitted frames finalized (see §6), timers invalidated by generation, metrics emitted.

DATA carries no epoch; old DATA is isolated by the new random ISN plus serial/window validation (accepted collision probability `W/2^32`, documented v1 limitation).

## 4. Sequence and reliability core

- Sequences are allocated only when a complete frame is atomically admitted to the session window (all `frame_count <= W` sequences at once). Serial (RFC 1982) arithmetic everywhere; every compared distance < 2^31.
- Cumulative ACK = last consumed sequence (`recv_next - 1`); before PONG the client sends no ACK/NACK. ACK is monotonic; stale/duplicate ACK ignored; future ACK or ACK across an unallocated sequence rejected and counted.
- SACK bit `i` describes `ACK+1+i` currently held. No-reneging: once reported held, a sequence is never reported missing; storage overflow drops new unreported packets.
- Every accepted out-of-order packet creates per-hole NACKs, throttled ≥ SRTT per hole, coalesced to ≤ 64 entries. Sender reacts to the first eligible NACK (SRTT suppression per packet, SACKed packets skipped, ABANDONed sequences answered with ABANDON only).
- RTO: RFC 6298-style estimation over cumulative-ACK samples of never-retransmitted packets (one sample per ACK, oldest covered packet); per-entry exponential backoff capped at RTO_MAX; G=1 ms (ADR-0011).
- Frame deadline: `capture_time + D` on the injected monotonic clock; `remaining_ms = ceil(max(0, D - age))`, saturated; admission early-exit when `remaining <= SRTT + 4*RTTVAR` (ADR-0004).
- ABANDON: deadline decision abandons the whole frame interval; payload freed, bounded stub retained, ranges normalized (sort, merge overlap/adjacency, split at 64 ranges, out-of-window rejected), idempotent at the receiver, retransmitted on the RTO backoff schedule until cumulatively ACKed. After ABANDON the sender never emits that frame's DATA again (INV-07).
- Frame dependencies and cascade abandon (ADR-0019): frames may declare a reference dependency (`depends_on_frame`). When a reference frame (KeyFrame) times out and is abandoned, all queued dependent frames in the send window are transitively cascade-abandoned, their sequence ranges coalesced into the outbound `ABANDON` datagram, and upstream signaled (`keyframe_requested`) to reset the GOP. Incoming DeltaFrames whose reference is already abandoned are pruned prior to admission (`PRE_ADMISSION_DROPPED`), shielding the send window from broken GOP congestion.

## 5. Receiver and playout

- DATA classification order: malformed → epoch/session mismatch → TOO_OLD → OUT_OF_WINDOW → DUPLICATE → descriptor conflict → VALID.
- Frame descriptor fixed by first accepted packet (`frame_count`, `rtp_timestamp`, derived sequence interval, format incl. `FRAME_END ⇔ index == frame_count-1`); conflict kills the frame (`PROTOCOL_VIOLATION`), no partial completion.
- Frame states `RECEIVING → COMPLETE | ABANDONED | EXPIRED`, exactly one terminal. EXPIRED is a delivery outcome only — it never advances `recv_next` (ADR-0006). Frame-id gaps are never awaited.
- Reorder storage 256 packets, drop-newest-unreported when full. Complete frames enter a 64-frame static jitter buffer with 60 ms playout delay over the RTP anchor (`local_anchor + (rtp_ts - rtp_anchor)/90000 + playout_delay`). Delivery at or before playout is ONTIME; after playout is LATE; incomplete at the playout scan is EXPIRED; dead frames are never delivered; buffer overflow drops the oldest not-yet-delivered frame (`playout_dropped`).

## 6. Metrics and accounting

Two ledgers, never mixed (ADR-0005):

```text
source_frames = capture_dropped + admitted_to_fanout
session_offered = ontime + late + abandoned + expired
                + pre_admission_dropped + in_flight          (per session and aggregate)
```

FDR = `session_ontime / session_offered`; the client separately reports playout-side ONTIME/LATE/EXPIRED/ABANDONED classification and latency percentiles. At the final snapshot every `IN_FLIGHT` entry closes as `ABANDONED(cause = SHUTDOWN | SESSION_TIMEOUT)`; the pre-close snapshot already satisfies the equation. Packet counters (duplicates, stale, malformed, NACKs, retransmissions, kernel drops) never alter frame-level counts. Metrics include session id, stream id, seed, clock mode, queue depths, memory high-water, watchdog violations, and teardown cause.

## 7. Resource bounds

Per spec §3.3 (ADR-0007): capture queue 256 frames (producer drop-newest); pending DATA = W packets; control queue 128 with ABANDON preserved; reorder 256; jitter 64; metric snapshots 32 (final never dropped); commands 64; timers 4096 (`RESOURCE_EXHAUSTED` on overflow). Scheduling: round-robin, ≤ 32 datagrams or 32 KiB per session turn; DATA token bucket 4 MiB/s / 64 KiB burst; control-only bucket 16 KiB (ADR-0008). Ownership table: ADR-0016.

## 8. Golden wire vectors

Fixed vectors (encode and decode) live in `tests/unit/wire_vectors.cpp`; the same hex strings are reproduced here for review.

```text
DATA   (44 B): 0101 0002 07 00 0001 000003e8 00000007 00005208 0061 0009 000a 0010
               80e0 1234 00005208 deadbeef  aabbccdd
ACK    (44 B): 0102 0000 07 00 0001 00000000 00000000 00000000 0000 0000 0000 0010
               00000422 0000000000000037 11223344
NACK   (44 B): 0103 0000 07 00 0001 00000000 00000000 00000000 0000 0000 0000 0010
               0002 0000 000003e9 000003eb 11223344
ABANDON(52 B): 0106 0000 07 00 0001 00000000 00000000 00000000 0000 0000 0000 0018
               0002 0000 000003e8 000003e9 000003ed 000003ed 11223344
PING J (40 B): 0104 0000 07 00 0001 00000000 00000000 00000000 0000 0000 0000 000c
               aabbccdd 01000000 55667788
PING K (44 B): 0104 0000 07 00 0001 00000000 00000000 00000000 0000 0000 0000 0010
               aabbccdd 02000000 11223344 55667788
PONG OK(44 B): 0105 0000 07 00 0001 00000000 00000000 00000000 0000 0000 0000 0010
               aabbccdd 00000000 99887766 0000bcda
```

## 9. Invariants

INV-01 unique sequence terminal state; INV-02 monotonic cumulative ACK; INV-03 SACK no-reneging; INV-04 bounded window, movement only via cumulative ACK; INV-05 descriptor consistency; INV-06 frame terminal uniqueness; INV-07 no post-abandon DATA; INV-08 assembly safety; INV-09 playout ordering; INV-10 Reactor-only protocol mutation; INV-11 exclusive queue ownership; INV-12 bounded queues; INV-13 safe SPSC publication; INV-14 stale-event safety; INV-15 atomic session visibility; INV-16 error-path terminality; INV-17 bounded waiting; INV-18 shutdown barrier; INV-19 final snapshot consistency; INV-20 FDR conservation; INV-21 packet/frame metric separation. Each has an owner and at least one test anchor (see `docs/invariants.md` for the full statement).

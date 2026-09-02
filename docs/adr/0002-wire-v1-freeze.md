# ADR-0002: Wire format v1 freeze (28-byte header + control payloads)

- **Status:** Accepted (Phase 0)
- **Context:** v0.2 left payload-length formulas, control payload layouts, epoch fields, and maximum datagram size open. The specification §4.1/§4.2 fixes them; this ADR records the frozen layout as the conformance target and names the fixed vectors.
- **Decision:** The datagram header is exactly 28 bytes, all integers network byte order, exactly as tabulated in spec §4.1 (version=1, header_len=7, DATA/ACK/NACK/PING/PONG/ABANDON, flags bit0 RETRANSMIT / bit1 FRAME_END / bit2 SYNC_START, all other bits zero). Control payloads follow spec §4.2 exactly; every control payload except Join PING carries `u32 session_epoch` as its final field. Size limits: media fragment ≤ 1200 B, DATA payload (RTP header + fragment) ≤ 1212 B, datagram ≤ 1240 B. Oversized datagrams, `MSG_TRUNC`, length inconsistencies, non-zero reserved fields, invalid flags/counts, unknown types, and trailing bytes are rejected and counted before any state mutation.
- **Options considered:** TLV extension headers, variable header length — rejected for v1: fixed layout makes vectors, fuzzing, and independent implementations tractable. Header length field retained for future extension but constrained to 7.
- **Consequences:** Golden encode/decode vectors live in `tests/unit/wire_vectors.cpp` and are reproduced in `docs/protocol.md`. Any wire change requires a new ADR, new vectors, and regression updates (spec §9).
- **Test anchors:** every malformed-vector unit case must leave protocol state unmutated.

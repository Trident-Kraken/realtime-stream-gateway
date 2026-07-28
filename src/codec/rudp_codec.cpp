// rudp_codec.cpp — 28-byte header + control payload codecs (ADR-0002).

#include "rudp_rt/wire.hpp"

#include <algorithm>
#include <cstring>

namespace rudp_rt {

void encode_header(std::byte* out, const Header& h) {
  out[0] = static_cast<std::byte>(h.version);
  out[1] = static_cast<std::byte>(h.type);
  store_u16(out + 2, h.flags);
  out[4] = static_cast<std::byte>(h.header_len);
  out[5] = static_cast<std::byte>(h.reserved);
  store_u16(out + 6, h.stream_id);
  store_u32(out + 8, h.seq);
  store_u32(out + 12, h.frame_id);
  store_u32(out + 16, h.rtp_timestamp);
  store_u16(out + 20, h.deadline_ms);
  store_u16(out + 22, h.frame_index);
  store_u16(out + 24, h.frame_count);
  store_u16(out + 26, h.payload_len);
}

DecodedHeader decode_header(std::span<const std::byte> datagram) {
  DecodedHeader out;
  if (datagram.size() < kHeaderSize) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  if (datagram.size() > kMaxDatagram) {
    out.reject = RejectReason::TOO_LARGE;
    return out;
  }
  Header h;
  h.version = static_cast<std::uint8_t>(datagram[0]);
  h.type = static_cast<std::uint8_t>(datagram[1]);
  h.flags = load_u16(datagram.data() + 2);
  h.header_len = static_cast<std::uint8_t>(datagram[4]);
  h.reserved = static_cast<std::uint8_t>(datagram[5]);
  h.stream_id = load_u16(datagram.data() + 6);
  h.seq = load_u32(datagram.data() + 8);
  h.frame_id = load_u32(datagram.data() + 12);
  h.rtp_timestamp = load_u32(datagram.data() + 16);
  h.deadline_ms = load_u16(datagram.data() + 20);
  h.frame_index = load_u16(datagram.data() + 22);
  h.frame_count = load_u16(datagram.data() + 24);
  h.payload_len = load_u16(datagram.data() + 26);

  if (h.version != kWireVersion) {
    out.reject = RejectReason::BAD_VERSION;
    return out;
  }
  if (h.type < static_cast<std::uint8_t>(PacketType::DATA) ||
      h.type > static_cast<std::uint8_t>(PacketType::ABANDON)) {
    out.reject = RejectReason::BAD_TYPE;
    return out;
  }
  if ((h.flags & ~kFlagKnownMask) != 0) {
    out.reject = RejectReason::BAD_FLAGS;
    return out;
  }
  if (h.header_len != kHeaderLenWords) {
    out.reject = RejectReason::BAD_HEADER_LEN;
    return out;
  }
  if (h.reserved != 0) {
    out.reject = RejectReason::BAD_RESERVED;
    return out;
  }
  if (h.stream_id == 0) {
    out.reject = RejectReason::BAD_STREAM_ID;
    return out;
  }
  if (h.payload_len != datagram.size() - kHeaderSize) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  const auto type = static_cast<PacketType>(h.type);
  if (type == PacketType::DATA) {
    if (h.payload_len == 0 || h.payload_len > kMaxDataPayload) {
      out.reject = RejectReason::LENGTH_MISMATCH;
      return out;
    }
    if (h.payload_len < kRtpHeaderSize) {
      out.reject = RejectReason::LENGTH_MISMATCH;
      return out;
    }
  } else {
    // Control packets must zero the DATA-only fields (spec §4.2).
    if (h.seq != 0 || h.frame_id != 0 || h.rtp_timestamp != 0 ||
        h.deadline_ms != 0 || h.frame_index != 0 || h.frame_count != 0) {
      out.reject = RejectReason::BAD_CONTROL_FORMAT;
      return out;
    }
    if (h.flags != 0) {
      out.reject = RejectReason::BAD_FLAGS;
      return out;
    }
  }
  out.header = h;
  return out;
}

// ---------------------------------------------------------------------------
// ACK
// ---------------------------------------------------------------------------

std::size_t encoded_ack_size() { return 16; }

void encode_ack(std::byte* out, const Ack& ack) {
  store_u32(out, ack.ack_number);
  store_u64(out + 4, ack.sack_bitmap);
  store_u32(out + 12, ack.session_epoch);
}

DecodedAck decode_ack(std::span<const std::byte> payload) {
  DecodedAck out;
  if (payload.size() != encoded_ack_size()) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  out.ack.ack_number = load_u32(payload.data());
  out.ack.sack_bitmap = load_u64(payload.data() + 4);
  out.ack.session_epoch = load_u32(payload.data() + 12);
  return out;
}

// ---------------------------------------------------------------------------
// NACK
// ---------------------------------------------------------------------------

std::size_t encoded_nack_size(std::size_t count) {
  return 4 + count * 4 + 4;
}

void encode_nack(std::byte* out, const Nack& nack) {
  const auto count = static_cast<std::uint16_t>(nack.seqs.size());
  store_u16(out, count);
  store_u16(out + 2, 0);
  for (std::size_t i = 0; i < nack.seqs.size(); ++i) {
    store_u32(out + 4 + i * 4, nack.seqs[i]);
  }
  store_u32(out + 4 + static_cast<std::size_t>(count) * 4, nack.session_epoch);
}

DecodedNack decode_nack(std::span<const std::byte> payload) {
  DecodedNack out;
  if (payload.size() < 8) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  const std::uint16_t count = load_u16(payload.data());
  const std::uint16_t reserved = load_u16(payload.data() + 2);
  if (reserved != 0) {
    out.reject = RejectReason::BAD_RESERVED;
    return out;
  }
  if (count > kMaxNackCount) {
    out.reject = RejectReason::BAD_COUNT;
    return out;
  }
  if (payload.size() != encoded_nack_size(count)) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  out.nack.seqs.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.nack.seqs[i] = load_u32(payload.data() + 4 + i * 4);
  }
  out.nack.session_epoch =
      load_u32(payload.data() + 4 + static_cast<std::size_t>(count) * 4);
  return out;
}

// ---------------------------------------------------------------------------
// ABANDON
// ---------------------------------------------------------------------------

std::size_t encoded_abandon_size(std::size_t range_count) {
  return 4 + range_count * 8 + 4;
}

void encode_abandon(std::byte* out, const EncodedAbandon& ab) {
  const auto count = static_cast<std::uint16_t>(ab.ranges.size());
  store_u16(out, count);
  store_u16(out + 2, 0);
  for (std::size_t i = 0; i < ab.ranges.size(); ++i) {
    store_u32(out + 4 + i * 8, ab.ranges[i].start);
    store_u32(out + 4 + i * 8 + 4, ab.ranges[i].end);
  }
  store_u32(out + 4 + static_cast<std::size_t>(count) * 8, ab.session_epoch);
}

DecodedAbandon decode_abandon(std::span<const std::byte> payload) {
  DecodedAbandon out;
  if (payload.size() < 8) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  const std::uint16_t count = load_u16(payload.data());
  const std::uint16_t reserved = load_u16(payload.data() + 2);
  if (reserved != 0) {
    out.reject = RejectReason::BAD_RESERVED;
    return out;
  }
  if (count > kMaxAbandonRanges) {
    out.reject = RejectReason::BAD_COUNT;
    return out;
  }
  if (payload.size() != encoded_abandon_size(count)) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  out.ab.ranges.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.ab.ranges[i].start = load_u32(payload.data() + 4 + i * 8);
    out.ab.ranges[i].end = load_u32(payload.data() + 4 + i * 8 + 4);
  }
  out.ab.session_epoch =
      load_u32(payload.data() + 4 + static_cast<std::size_t>(count) * 8);
  return out;
}

// ---------------------------------------------------------------------------
// PING / PONG
// ---------------------------------------------------------------------------

std::size_t encoded_ping_size(PingPurpose purpose) {
  return purpose == PingPurpose::JOIN ? 12 : 16;
}

void encode_ping(std::byte* out, const Ping& ping) {
  store_u32(out, ping.nonce);
  out[4] = static_cast<std::byte>(ping.purpose);
  out[5] = std::byte{0};
  store_u16(out + 6, 0);
  if (ping.purpose == PingPurpose::JOIN) {
    store_u32(out + 8, ping.client_epoch);
  } else {
    store_u32(out + 8, ping.session_epoch);
    store_u32(out + 12, ping.client_epoch);
  }
}

DecodedPing decode_ping(std::span<const std::byte> payload) {
  DecodedPing out;
  if (payload.size() < 12) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  const std::uint8_t purpose_raw = static_cast<std::uint8_t>(payload[4]);
  if (purpose_raw < static_cast<std::uint8_t>(PingPurpose::JOIN) ||
      purpose_raw > static_cast<std::uint8_t>(PingPurpose::RESTART)) {
    out.reject = RejectReason::BAD_CONTROL_FORMAT;
    return out;
  }
  const auto purpose = static_cast<PingPurpose>(purpose_raw);
  const std::uint8_t r5 = static_cast<std::uint8_t>(payload[5]);
  const std::uint16_t r6 = load_u16(payload.data() + 6);
  if (r5 != 0 || r6 != 0) {
    out.reject = RejectReason::BAD_RESERVED;
    return out;
  }
  const std::size_t expected = encoded_ping_size(purpose);
  if (payload.size() != expected) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  out.ping.nonce = load_u32(payload.data());
  out.ping.purpose = purpose;
  if (purpose == PingPurpose::JOIN) {
    out.ping.client_epoch = load_u32(payload.data() + 8);
  } else {
    out.ping.session_epoch = load_u32(payload.data() + 8);
    out.ping.client_epoch = load_u32(payload.data() + 12);
  }
  return out;
}

std::size_t encoded_pong_size() { return 16; }

void encode_pong(std::byte* out, const Pong& pong) {
  store_u32(out, pong.nonce);
  out[4] = static_cast<std::byte>(pong.status);
  out[5] = std::byte{0};
  store_u16(out + 6, 0);
  store_u32(out + 8, pong.session_epoch);
  store_u32(out + 12, pong.server_isn);
}

DecodedPong decode_pong(std::span<const std::byte> payload) {
  DecodedPong out;
  if (payload.size() != encoded_pong_size()) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  const std::uint8_t status_raw = static_cast<std::uint8_t>(payload[4]);
  if (status_raw > static_cast<std::uint8_t>(PongStatus::REJECT_STALE)) {
    out.reject = RejectReason::BAD_CONTROL_FORMAT;
    return out;
  }
  const std::uint8_t r5 = static_cast<std::uint8_t>(payload[5]);
  const std::uint16_t r6 = load_u16(payload.data() + 6);
  if (r5 != 0 || r6 != 0) {
    out.reject = RejectReason::BAD_RESERVED;
    return out;
  }
  out.pong.nonce = load_u32(payload.data());
  out.pong.status = static_cast<PongStatus>(status_raw);
  out.pong.session_epoch = load_u32(payload.data() + 8);
  out.pong.server_isn = load_u32(payload.data() + 12);
  return out;
}

// ---------------------------------------------------------------------------
// RTP
// ---------------------------------------------------------------------------

void encode_rtp_header(std::byte* out, const RtpHeader& rtp) {
  out[0] = std::byte{0x80};  // V=2, P=0, X=0, CC=0
  out[1] = static_cast<std::byte>((rtp.marker ? 0x80 : 0x00) | kRtpPayloadType);
  store_u16(out + 2, rtp.sequence);
  store_u32(out + 4, rtp.timestamp);
  store_u32(out + 8, rtp.ssrc);
}

DecodedRtp decode_rtp_header(std::span<const std::byte> payload) {
  DecodedRtp out;
  if (payload.size() < kRtpHeaderSize) {
    out.reject = RejectReason::LENGTH_MISMATCH;
    return out;
  }
  const std::uint8_t b0 = static_cast<std::uint8_t>(payload[0]);
  const std::uint8_t b1 = static_cast<std::uint8_t>(payload[1]);
  if ((b0 >> 6) != 2) {
    out.reject = RejectReason::BAD_VERSION;
    return out;
  }
  if ((b0 & 0x30) != 0 || (b0 & 0x0F) != 0) {  // P/X and CC must be zero
    out.reject = RejectReason::BAD_CONTROL_FORMAT;
    return out;
  }
  if ((b1 & 0x7F) != kRtpPayloadType) {
    out.reject = RejectReason::BAD_CONTROL_FORMAT;
    return out;
  }
  out.rtp.marker = (b1 & 0x80) != 0;
  out.rtp.sequence = load_u16(payload.data() + 2);
  out.rtp.timestamp = load_u32(payload.data() + 4);
  out.rtp.ssrc = load_u32(payload.data() + 8);
  return out;
}

// ---------------------------------------------------------------------------
// DATA builder
// ---------------------------------------------------------------------------

std::optional<Datagram> build_data_datagram(const Endpoint& peer,
                                            const Header& base,
                                            const RtpHeader& rtp,
                                            std::span<const std::byte> fragment) {
  if (fragment.size() > kMaxMediaFragment) return std::nullopt;
  Datagram dg;
  dg.peer = peer;
  dg.bytes.resize(kHeaderSize + kRtpHeaderSize + fragment.size());
  Header h = base;
  h.version = kWireVersion;
  h.type = static_cast<std::uint8_t>(PacketType::DATA);
  h.header_len = kHeaderLenWords;
  h.reserved = 0;
  h.payload_len = static_cast<std::uint16_t>(kRtpHeaderSize + fragment.size());
  encode_header(dg.bytes.data(), h);
  encode_rtp_header(dg.bytes.data() + kHeaderSize, rtp);
  if (!fragment.empty()) {
    std::memcpy(dg.bytes.data() + kHeaderSize + kRtpHeaderSize,
                fragment.data(), fragment.size());
  }
  return dg;
}

std::vector<AbandonRange> normalize_abandon_ranges(
    std::vector<AbandonRange> ranges) {
  ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                              [](const AbandonRange& r) {
                                return seq_gt(r.start, r.end);
                              }),
               ranges.end());
  std::sort(ranges.begin(), ranges.end(),
            [](const AbandonRange& a, const AbandonRange& b) {
              return seq_lt(a.start, b.start);
            });
  std::vector<AbandonRange> out;
  for (const auto& r : ranges) {
    if (!out.empty()) {
      AbandonRange& last = out.back();
      const std::uint32_t last_end_plus = seq_next(last.end);
      if (!seq_lt(last_end_plus, r.start)) {
        if (seq_gt(r.end, last.end)) last.end = r.end;
        continue;
      }
    }
    out.push_back(r);
  }
  return out;
}

}  // namespace rudp_rt

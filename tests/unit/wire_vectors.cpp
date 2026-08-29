// wire_vectors.cpp — golden encode/decode vectors (ADR-0002, spec §4.1–§4.2).
// The hex strings here are reproduced in docs/protocol.md §8.

#include "test_framework.hpp"

#include "rudp_rt/wire.hpp"

#include <string>
#include <vector>

namespace {

std::string to_hex(const std::vector<std::byte>& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::byte b : bytes) {
    const unsigned v = static_cast<unsigned>(b);
    out += digits[(v >> 4) & 0xF];
    out += digits[v & 0xF];
  }
  return out;
}

std::vector<std::byte> from_hex(const std::string& hex) {
  std::vector<std::byte> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const auto nibble = [](char c) {
      if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
      return static_cast<int>(c - 'a' + 10);
    };
    out.push_back(static_cast<std::byte>(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
  }
  return out;
}

}  // namespace

namespace rudp_test {

using namespace rudp_rt;

RUDP_TEST(data_vector_roundtrip) {
  // stream=1, seq=1000, frame=7, rtp_ts=21000, deadline=97ms, index=9/10,
  // flags=FRAME_END, RTP(seq=0x1234, ts=21000, ssrc=0xDEADBEEF, marker=1),
  // media = AA BB CC DD.
  const std::string golden =
      "0101" "0002" "07" "00" "0001" "000003e8" "00000007" "00005208" "0061"
      "0009" "000a" "0010"
      "80e0" "1234" "00005208" "deadbeef" "aabbccdd";

  Header h;
  h.version = 1;
  h.type = static_cast<std::uint8_t>(PacketType::DATA);
  h.flags = kFlagFrameEnd;
  h.header_len = 7;
  h.stream_id = 1;
  h.seq = 1000;
  h.frame_id = 7;
  h.rtp_timestamp = 21000;
  h.deadline_ms = 97;
  h.frame_index = 9;
  h.frame_count = 10;
  h.payload_len = 16;

  RtpHeader rtp;
  rtp.sequence = 0x1234;
  rtp.timestamp = 21000;
  rtp.ssrc = 0xDEADBEEFu;
  rtp.marker = true;

  std::vector<std::byte> fragment = {std::byte{0xAA}, std::byte{0xBB},
                                     std::byte{0xCC}, std::byte{0xDD}};
  auto dg = build_data_datagram(Endpoint{1, 1}, h, rtp, fragment);
  RUDP_CHECK(dg.has_value());
  RUDP_CHECK_EQ(to_hex(dg->bytes), golden);

  // Decode side: structural validity + field equality.
  auto decoded = decode_header(dg->bytes);
  RUDP_CHECK(decoded.reject == RejectReason::NONE);
  RUDP_CHECK_EQ(decoded.header.seq, 1000u);
  RUDP_CHECK_EQ(decoded.header.frame_id, 7u);
  RUDP_CHECK_EQ(decoded.header.rtp_timestamp, 21000u);
  RUDP_CHECK_EQ(decoded.header.deadline_ms, 97u);
  RUDP_CHECK_EQ(decoded.header.frame_index, 9u);
  RUDP_CHECK_EQ(decoded.header.frame_count, 10u);
  RUDP_CHECK_EQ(decoded.header.payload_len, 16u);
  auto rtp_dec = decode_rtp_header(
      std::span<const std::byte>(dg->bytes).subspan(kHeaderSize));
  RUDP_CHECK(rtp_dec.reject == RejectReason::NONE);
  RUDP_CHECK(rtp_dec.rtp.marker);
  RUDP_CHECK_EQ(rtp_dec.rtp.sequence, 0x1234u);
  RUDP_CHECK_EQ(rtp_dec.rtp.ssrc, 0xDEADBEEFu);
}

RUDP_TEST(ack_vector_roundtrip) {
  // ack_number=0x422, bitmap=0x37 (bits 0,1,2,4,5), epoch=0x11223344.
  const std::string golden =
      "0102" "0000" "07" "00" "0001" "00000000" "00000000" "00000000" "0000"
      "0000" "0000" "0010"
      "00000422" "0000000000000037" "11223344";

  Datagram dg;
  dg.bytes.assign(kHeaderSize + encoded_ack_size(), std::byte{0});
  Header h;
  h.version = 1;
  h.header_len = 7;
  h.type = static_cast<std::uint8_t>(PacketType::ACK);
  h.stream_id = 1;
  h.payload_len = static_cast<std::uint16_t>(encoded_ack_size());
  encode_header(dg.bytes.data(), h);
  Ack ack;
  ack.ack_number = 0x422;
  ack.sack_bitmap = 0x37;
  ack.session_epoch = 0x11223344;
  encode_ack(dg.bytes.data() + kHeaderSize, ack);

  RUDP_CHECK_EQ(to_hex(dg.bytes), golden);
  auto decoded = decode_header(dg.bytes);
  RUDP_CHECK(decoded.reject == RejectReason::NONE);
  auto payload = decode_ack(std::span<const std::byte>(dg.bytes).subspan(kHeaderSize));
  RUDP_CHECK(payload.reject == RejectReason::NONE);
  RUDP_CHECK_EQ(payload.ack.ack_number, 0x422u);
  RUDP_CHECK_EQ(payload.ack.sack_bitmap, 0x37ull);
  RUDP_CHECK_EQ(payload.ack.session_epoch, 0x11223344u);
}

RUDP_TEST(nack_vector_roundtrip) {
  // count=2, seqs=[1001, 1003], epoch=0x11223344.
  const std::string golden =
      "0103" "0000" "07" "00" "0001" "00000000" "00000000" "00000000" "0000"
      "0000" "0000" "0010"
      "0002" "0000" "000003e9" "000003eb" "11223344";

  Datagram dg;
  dg.bytes.assign(kHeaderSize + encoded_nack_size(2), std::byte{0});
  Header h;
  h.version = 1;
  h.header_len = 7;
  h.type = static_cast<std::uint8_t>(PacketType::NACK);
  h.stream_id = 1;
  h.payload_len = static_cast<std::uint16_t>(encoded_nack_size(2));
  encode_header(dg.bytes.data(), h);
  Nack nack;
  nack.session_epoch = 0x11223344;
  nack.seqs = {1001, 1003};
  encode_nack(dg.bytes.data() + kHeaderSize, nack);

  RUDP_CHECK_EQ(to_hex(dg.bytes), golden);
  auto payload = decode_nack(std::span<const std::byte>(dg.bytes).subspan(kHeaderSize));
  RUDP_CHECK(payload.reject == RejectReason::NONE);
  RUDP_CHECK_EQ(payload.nack.seqs.size(), 2u);
  RUDP_CHECK_EQ(payload.nack.seqs[0], 1001u);
  RUDP_CHECK_EQ(payload.nack.seqs[1], 1003u);
  RUDP_CHECK_EQ(payload.nack.session_epoch, 0x11223344u);
}

RUDP_TEST(abandon_vector_roundtrip) {
  // range_count=2, ranges=[(1000,1001),(1005,1005)], epoch=0x11223344.
  const std::string golden =
      "0106" "0000" "07" "00" "0001" "00000000" "00000000" "00000000" "0000"
      "0000" "0000" "0018"
      "0002" "0000" "000003e8" "000003e9" "000003ed" "000003ed" "11223344";

  Datagram dg;
  dg.bytes.assign(kHeaderSize + encoded_abandon_size(2), std::byte{0});
  Header h;
  h.version = 1;
  h.header_len = 7;
  h.type = static_cast<std::uint8_t>(PacketType::ABANDON);
  h.stream_id = 1;
  h.payload_len = static_cast<std::uint16_t>(encoded_abandon_size(2));
  encode_header(dg.bytes.data(), h);
  EncodedAbandon ab;
  ab.session_epoch = 0x11223344;
  ab.ranges = {{1000, 1001}, {1005, 1005}};
  encode_abandon(dg.bytes.data() + kHeaderSize, ab);

  RUDP_CHECK_EQ(to_hex(dg.bytes), golden);
  auto payload =
      decode_abandon(std::span<const std::byte>(dg.bytes).subspan(kHeaderSize));
  RUDP_CHECK(payload.reject == RejectReason::NONE);
  RUDP_CHECK_EQ(payload.ab.ranges.size(), 2u);
  RUDP_CHECK((payload.ab.ranges[0] == AbandonRange{1000, 1001}));
  RUDP_CHECK((payload.ab.ranges[1] == AbandonRange{1005, 1005}));
}

RUDP_TEST(ping_vectors_roundtrip) {
  // JOIN: nonce 0xAABBCCDD, client_epoch 0x55667788.
  const std::string join_golden =
      "0104" "0000" "07" "00" "0001" "00000000" "00000000" "00000000" "0000"
      "0000" "0000" "000c"
      "aabbccdd" "01000000" "55667788";
  // KEEPALIVE: nonce, purpose=2, session_epoch=0x11223344, client_epoch.
  const std::string ka_golden =
      "0104" "0000" "07" "00" "0001" "00000000" "00000000" "00000000" "0000"
      "0000" "0000" "0010"
      "aabbccdd" "02000000" "11223344" "55667788";

  {
    Datagram dg;
    Ping ping;
    ping.nonce = 0xAABBCCDDu;
    ping.purpose = PingPurpose::JOIN;
    ping.client_epoch = 0x55667788u;
    dg.bytes.assign(kHeaderSize + encoded_ping_size(ping.purpose), std::byte{0});
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::PING);
    h.stream_id = 1;
    h.payload_len = static_cast<std::uint16_t>(encoded_ping_size(ping.purpose));
    encode_header(dg.bytes.data(), h);
    encode_ping(dg.bytes.data() + kHeaderSize, ping);
    RUDP_CHECK_EQ(to_hex(dg.bytes), join_golden);
    auto p = decode_ping(std::span<const std::byte>(dg.bytes).subspan(kHeaderSize));
    RUDP_CHECK(p.reject == RejectReason::NONE);
    RUDP_CHECK(p.ping.purpose == PingPurpose::JOIN);
    RUDP_CHECK_EQ(p.ping.client_epoch, 0x55667788u);
  }
  {
    Datagram dg;
    Ping ping;
    ping.nonce = 0xAABBCCDDu;
    ping.purpose = PingPurpose::KEEPALIVE;
    ping.session_epoch = 0x11223344u;
    ping.client_epoch = 0x55667788u;
    dg.bytes.assign(kHeaderSize + encoded_ping_size(ping.purpose), std::byte{0});
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::PING);
    h.stream_id = 1;
    h.payload_len = static_cast<std::uint16_t>(encoded_ping_size(ping.purpose));
    encode_header(dg.bytes.data(), h);
    encode_ping(dg.bytes.data() + kHeaderSize, ping);
    RUDP_CHECK_EQ(to_hex(dg.bytes), ka_golden);
    auto p = decode_ping(std::span<const std::byte>(dg.bytes).subspan(kHeaderSize));
    RUDP_CHECK(p.reject == RejectReason::NONE);
    RUDP_CHECK(p.ping.purpose == PingPurpose::KEEPALIVE);
    RUDP_CHECK_EQ(p.ping.session_epoch, 0x11223344u);
  }
}

RUDP_TEST(pong_vector_roundtrip) {
  // status OK, epoch 0x99887766, server_isn 0xBCDA.
  const std::string golden =
      "0105" "0000" "07" "00" "0001" "00000000" "00000000" "00000000" "0000"
      "0000" "0000" "0010"
      "aabbccdd" "00000000" "99887766" "0000bcda";

  Datagram dg;
  dg.bytes.assign(kHeaderSize + encoded_pong_size(), std::byte{0});
  Header h;
  h.version = 1;
  h.header_len = 7;
  h.type = static_cast<std::uint8_t>(PacketType::PONG);
  h.stream_id = 1;
  h.payload_len = static_cast<std::uint16_t>(encoded_pong_size());
  encode_header(dg.bytes.data(), h);
  Pong pong;
  pong.nonce = 0xAABBCCDDu;
  pong.status = PongStatus::OK;
  pong.session_epoch = 0x99887766u;
  pong.server_isn = 0xBCDAu;
  encode_pong(dg.bytes.data() + kHeaderSize, pong);

  RUDP_CHECK_EQ(to_hex(dg.bytes), golden);
  auto p = decode_pong(std::span<const std::byte>(dg.bytes).subspan(kHeaderSize));
  RUDP_CHECK(p.reject == RejectReason::NONE);
  RUDP_CHECK(p.pong.status == PongStatus::OK);
  RUDP_CHECK_EQ(p.pong.session_epoch, 0x99887766u);
  RUDP_CHECK_EQ(p.pong.server_isn, 0xBCDAu);
}

RUDP_TEST(malformed_vectors_rejected_without_mutation) {
  const std::vector<std::pair<std::string, RejectReason>> cases = {
      {"0201" "0000" "07" "00" "0001" "0000000000000000000000000000" "0000"
       "0000" "0000" "0000",
       RejectReason::BAD_VERSION},
      {"01ff" "0000" "07" "00" "0001" "0000000000000000000000000000" "0000"
       "0000" "0000" "0000",
       RejectReason::BAD_TYPE},
      {"0101" "0008" "07" "00" "0001" "0000000000000000000000000000" "0000"
       "0000" "0000" "0000",
       RejectReason::BAD_FLAGS},
      {"0101" "0000" "06" "00" "0001" "0000000000000000000000000000" "0000"
       "0000" "0000" "0000",
       RejectReason::BAD_HEADER_LEN},
      {"0101" "0000" "07" "01" "0001" "0000000000000000000000000000" "0000"
       "0000" "0000" "0000",
       RejectReason::BAD_RESERVED},
      {"0101" "0000" "07" "00" "0000" "0000000000000000000000000000" "0000"
       "0000" "0000" "0000",
       RejectReason::BAD_STREAM_ID},
      // ACK with non-zero seq (control packets must zero DATA-only fields).
      {"0102" "0000" "07" "00" "0001" "00000001" "0000000000000000"
       "00000000" "0000" "0010"
       "00000422" "0000000000000037" "11223344",
       RejectReason::BAD_CONTROL_FORMAT},
  };
  for (const auto& [hex, expected] : cases) {
    const auto bytes = from_hex(hex);
    auto decoded = decode_header(bytes);
    RUDP_CHECK_EQ(static_cast<int>(decoded.reject), static_cast<int>(expected));
  }
}

}  // namespace rudp_test

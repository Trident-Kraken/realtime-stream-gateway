#pragma once
// rudp_rt/wire.hpp — header/control/RTP codec contracts (ADR-0002).
//
// Every encode produces a fixed-layout datagram; every decode validates the
// full structural contract and reports a RejectReason without side effects.
// Golden vectors: tests/unit/wire_vectors.cpp and docs/protocol.md §8.

#include "rudp_rt/types.hpp"

namespace rudp_rt {

constexpr std::size_t kHeaderSize = 28;
constexpr std::size_t kRtpHeaderSize = 12;
constexpr std::size_t kMaxMediaFragment = 1200;
constexpr std::size_t kMaxDataPayload = kRtpHeaderSize + kMaxMediaFragment;  // 1212
constexpr std::size_t kMaxDatagram = kHeaderSize + kMaxDataPayload;          // 1240
constexpr std::size_t kMaxNackCount = 64;
constexpr std::size_t kMaxAbandonRanges = 64;
constexpr std::uint8_t kWireVersion = 1;
constexpr std::uint8_t kHeaderLenWords = 7;
constexpr std::uint8_t kRtpPayloadType = 96;

// ---------------------------------------------------------------------------
// Value types exchanged between the receive pipeline stages
// (UDP -> Receiver -> FrameAssembler -> JitterBuffer -> Consumer).
// ---------------------------------------------------------------------------

// A DATA packet accepted by the receiver, handed to the frame assembler.
struct AcceptedPacket {
  std::uint32_t seq = 0;
  std::uint32_t frame_id = 0;
  std::uint32_t rtp_timestamp = 0;
  std::uint16_t stream_id = 0;
  std::uint16_t frame_index = 0;
  std::uint16_t frame_count = 0;
  std::uint16_t rtp_seq = 0;
  bool retransmit = false;
  bool frame_end = false;
  bool sync_start = false;
  std::uint16_t deadline_ms = 0;
  MonoNs arrival = 0;             // set by the receiver (injected clock)
  std::vector<std::byte> media;   // fragment bytes without the RTP header
};

// A fully assembled frame with its bytes concatenated in packet order.
struct CompletedFrame {
  std::uint16_t stream_id = 0;
  std::uint32_t frame_id = 0;
  std::uint32_t rtp_timestamp = 0;
  std::uint16_t frame_count = 0;
  MonoNs first_arrival = 0;
  MonoNs last_arrival = 0;
  std::vector<std::byte> bytes;
};

// ControlOutput: what a Receiver produces for one input datagram (spec §5:
// "Receiver ... return value-owned datagrams/events").
struct ControlOutput {
  std::vector<Datagram> datagrams;      // ACK/NACK replies, ready to send
  std::vector<AcceptedPacket> accepted; // packets consumed by the assembler
};

// RTP header value type (v0.2 D-03: media semantics separate from transport).
struct RtpHeader {
  std::uint16_t sequence = 0;
  std::uint32_t timestamp = 0;
  std::uint32_t ssrc = 0;
  bool marker = false;
};

// ---------------------------------------------------------------------------
// Byte-order helpers (also used by tests)
// ---------------------------------------------------------------------------
constexpr std::uint16_t load_u16(const std::byte* p) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) << 8 |
                                    static_cast<std::uint16_t>(p[1]));
}
constexpr std::uint32_t load_u32(const std::byte* p) {
  return static_cast<std::uint32_t>(static_cast<std::uint32_t>(p[0]) << 24 |
                                    static_cast<std::uint32_t>(p[1]) << 16 |
                                    static_cast<std::uint32_t>(p[2]) << 8 |
                                    static_cast<std::uint32_t>(p[3]));
}
constexpr std::uint64_t load_u64(const std::byte* p) {
  return (static_cast<std::uint64_t>(load_u32(p)) << 32) | load_u32(p + 4);
}
constexpr void store_u16(std::byte* p, std::uint16_t v) {
  p[0] = static_cast<std::byte>(v >> 8);
  p[1] = static_cast<std::byte>(v);
}
constexpr void store_u32(std::byte* p, std::uint32_t v) {
  p[0] = static_cast<std::byte>(v >> 24);
  p[1] = static_cast<std::byte>(v >> 16);
  p[2] = static_cast<std::byte>(v >> 8);
  p[3] = static_cast<std::byte>(v);
}
constexpr void store_u64(std::byte* p, std::uint64_t v) {
  store_u32(p, static_cast<std::uint32_t>(v >> 32));
  store_u32(p + 4, static_cast<std::uint32_t>(v));
}

// ---------------------------------------------------------------------------
// Header codec
// ---------------------------------------------------------------------------

void encode_header(std::byte* out, const Header& h);

struct DecodedHeader {
  Header header;
  RejectReason reject = RejectReason::NONE;  // NONE means structurally valid
};

// Validates: size, version, header_len, reserved, known type, flags mask,
// payload_len bound for the type. Control-format checks (counts, trailing
// bytes, zeroed fields) happen in the payload decoders below.
DecodedHeader decode_header(std::span<const std::byte> datagram);

// ---------------------------------------------------------------------------
// Payload codecs. Each encode appends to `out` (expected size reserved by the
// caller). Each decode consumes exactly the documented byte count; trailing
// bytes are rejected.
// ---------------------------------------------------------------------------

std::size_t encoded_ack_size();
void encode_ack(std::byte* out, const Ack& ack);
struct DecodedAck { Ack ack; RejectReason reject = RejectReason::NONE; };
DecodedAck decode_ack(std::span<const std::byte> payload);

std::size_t encoded_nack_size(std::size_t count);
void encode_nack(std::byte* out, const Nack& nack);
struct DecodedNack { Nack nack; RejectReason reject = RejectReason::NONE; };
DecodedNack decode_nack(std::span<const std::byte> payload);

struct EncodedAbandon {
  std::uint32_t session_epoch = 0;
  std::vector<AbandonRange> ranges;  // <= 64 after split by the sender
};
std::size_t encoded_abandon_size(std::size_t range_count);
void encode_abandon(std::byte* out, const EncodedAbandon& ab);
struct DecodedAbandon { EncodedAbandon ab; RejectReason reject = RejectReason::NONE; };
DecodedAbandon decode_abandon(std::span<const std::byte> payload);

std::size_t encoded_ping_size(PingPurpose purpose);
void encode_ping(std::byte* out, const Ping& ping);
struct DecodedPing { Ping ping; RejectReason reject = RejectReason::NONE; };
DecodedPing decode_ping(std::span<const std::byte> payload);

std::size_t encoded_pong_size();
void encode_pong(std::byte* out, const Pong& pong);
struct DecodedPong { Pong pong; RejectReason reject = RejectReason::NONE; };
DecodedPong decode_pong(std::span<const std::byte> payload);

// ---------------------------------------------------------------------------
// RTP codec
// ---------------------------------------------------------------------------

void encode_rtp_header(std::byte* out, const RtpHeader& rtp);
struct DecodedRtp {
  RtpHeader rtp;
  RejectReason reject = RejectReason::NONE;  // rejects padding/extension/CSRC/PT
};
DecodedRtp decode_rtp_header(std::span<const std::byte> payload);

// ---------------------------------------------------------------------------
// Whole-datagram builders (DATA path helpers)
// ---------------------------------------------------------------------------

// Builds a complete DATA datagram: 28-byte header + RTP header + fragment.
// Returns std::nullopt if the fragment exceeds kMaxMediaFragment.
std::optional<Datagram> build_data_datagram(const Endpoint& peer,
                                            const Header& base,
                                            const RtpHeader& rtp,
                                            std::span<const std::byte> fragment);

// Normalizes ABANDON ranges: drops invalid (start > end), sorts by serial
// start, merges overlapping and adjacent ranges (spec §4.5).
std::vector<AbandonRange> normalize_abandon_ranges(
    std::vector<AbandonRange> ranges);

}  // namespace rudp_rt

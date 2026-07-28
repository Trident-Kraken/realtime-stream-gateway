// rtp_codec.cpp — RTP profile helpers for the v1 frozen profile (spec §2.3).
//
// The codec itself lives in wire.hpp (encode_rtp_header/decode_rtp_header);
// this translation unit owns the per-stream RTP sequencing state that pairs
// with the packetizer: one RTP sequence per (frame, packet index), marker on
// the final packet, timestamp = frame rtp_timestamp.

#include "rudp_rt/wire.hpp"

namespace rudp_rt {

// Validates a DATA payload against the RUDP header copy (spec §4.1:
// "RTP fields are validated before a DATA packet is committed").
RejectReason validate_data_payload(const Header& h,
                                   std::span<const std::byte> payload) {
  auto decoded = decode_rtp_header(payload);
  if (decoded.reject != RejectReason::NONE) return decoded.reject;
  if (decoded.rtp.timestamp != h.rtp_timestamp) {
    return RejectReason::BAD_CONTROL_FORMAT;  // timestamp copy mismatch
  }
  return RejectReason::NONE;
}

}  // namespace rudp_rt

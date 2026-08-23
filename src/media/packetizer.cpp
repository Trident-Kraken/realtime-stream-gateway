// packetizer.cpp — MediaFrame -> PacketizedFrame fragmentation (spec §2.3).

#include "rudp_rt/sender.hpp"

#include <algorithm>

namespace rudp_rt {

// ---------------------------------------------------------------------------
// Packetizer
// ---------------------------------------------------------------------------

std::optional<PacketizedFrame> Packetizer::packetize(MediaFrame&& frame,
                                                     std::uint32_t ssrc,
                                                     int media_payload_bytes,
                                                     int max_frame_packets) {
  const std::size_t frag_cap = static_cast<std::size_t>(media_payload_bytes);
  const std::size_t count = frame.bytes.empty()
                                ? 1
                                : (frame.bytes.size() + frag_cap - 1) / frag_cap;
  if (count > static_cast<std::size_t>(max_frame_packets)) return std::nullopt;

  PacketizedFrame pf;
  pf.stream_id = frame.stream_id;
  pf.frame_id = frame.frame_id;
  pf.rtp_timestamp = frame.rtp_timestamp;
  pf.ssrc = ssrc;
  pf.capture_time = frame.capture_time;
  pf.deadline_budget = frame.deadline_budget;
  pf.frame_type = frame.frame_type;
  pf.depends_on_frame = frame.depends_on_frame;
  pf.bytes = std::move(frame.bytes);

  pf.fragments.resize(count);
  for (std::size_t i = 0; i < pf.fragments.size(); ++i) {
    auto& frag = pf.fragments[i];
    frag.index = static_cast<std::uint16_t>(i);
    frag.rtp_seq = rtp_seq_++;
    frag.offset = static_cast<std::uint32_t>(i * frag_cap);
    frag.len = static_cast<std::uint32_t>(
        std::min(frag_cap, pf.bytes.size() - frag.offset));
    frag.frame_end = (i + 1 == pf.fragments.size());
  }
  return pf;
}


}  // namespace rudp_rt

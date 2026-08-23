// synthetic_source.cpp — deterministic synthetic media source (spec §2.3).

#include "rudp_rt/media.hpp"

namespace rudp_rt {

void SyntheticSource::fill_pattern(std::vector<std::byte>& bytes,
                                   std::uint32_t frame_id,
                                   std::uint16_t stream_id) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const std::uint32_t x = frame_id * 2654435769u +
                            static_cast<std::uint32_t>(i) * 2246822519u +
                            static_cast<std::uint32_t>(stream_id);
    bytes[i] = static_cast<std::byte>((x >> 13) & 0xFFu);
  }
}

}  // namespace rudp_rt

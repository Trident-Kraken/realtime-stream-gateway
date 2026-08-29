// metrics.cpp — snapshot aggregation helpers.

#include "rudp_rt/metrics.hpp"

namespace rudp_rt {

std::uint64_t session_offered_total(const MetricsSnapshot& s) {
  std::uint64_t total = 0;
  for (const auto& sm : s.sessions) total += sm.frames.offered();
  return total;
}

double session_fdr(const MetricsSnapshot& s) {
  const std::uint64_t offered = session_offered_total(s);
  if (offered == 0) return 0.0;
  std::uint64_t ontime = 0;
  for (const auto& sm : s.sessions) ontime += sm.frames.ontime;
  return static_cast<double>(ontime) / static_cast<double>(offered);
}

}  // namespace rudp_rt

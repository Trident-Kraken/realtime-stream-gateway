// matrix.cpp — fault-injection rows (spec §8.2). Each row runs the seeded
// virtual-clock scenario and asserts: FDR conservation holds continuously,
// zero watchdog violations, and records the row's outcome. The full matrix
// with production frame counts runs via tools/run_fault_matrix.sh
// (rudp_sim); these ctest rows are the fast regression subset.

#include "../sim_fixture.hpp"

namespace rudp_test {

using namespace rudp_rt;

namespace {

void run_row(const char*, double data_loss, int burst, MonoNs jitter,
             double reorder, double duplicate, double control_loss,
             bool down_only) {
  SimSession s(1, 0x5EED);
  if (down_only) {
    // Loss only on the DATA path; the control path stays clean.
    DirectionFaults down;
    down.data_loss = data_loss;
    down.burst_length = burst;
    down.jitter = jitter;
    down.reorder = reorder;
    down.duplicate = duplicate;
    s.net.set_route(s.gw_sock.local(), down);
  } else {
    s.set_faults(data_loss, burst, jitter, reorder, duplicate, control_loss);
  }
  s.run(ns_from_ms(2500));
  s.shutdown_close();
  s.assert_invariants();

  // Row summary for the log (visible with --case output).
  std::uint64_t offered = 0, terminal = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    offered += sm.frames.offered();
    terminal += sm.frames.ontime + sm.frames.late + sm.frames.abandoned +
                sm.frames.expired + sm.frames.pre_admission_dropped;
  }
  RUDP_CHECK_EQ(offered, terminal);  // close-out: nothing in flight remains
}

}  // namespace

RUDP_TEST(fault_iid_0pct) { run_row("iid0", 0.0, 0, 0, 0, 0, 0.0, true); }
RUDP_TEST(fault_iid_1pct) { run_row("iid1", 0.01, 0, 0, 0, 0, 0.0, true); }
RUDP_TEST(fault_iid_5pct) { run_row("iid5", 0.05, 0, 0, 0, 0, 0.0, true); }
RUDP_TEST(fault_iid_10pct) { run_row("iid10", 0.10, 0, 0, 0, 0, 0.0, true); }
RUDP_TEST(fault_iid_20pct) { run_row("iid20", 0.20, 0, 0, 0, 0, 0.0, true); }
RUDP_TEST(fault_burst_len2) { run_row("burst2", 0.20, 2, 0, 0, 0, 0.0, true); }
RUDP_TEST(fault_burst_len4) { run_row("burst4", 0.20, 4, 0, 0, 0, 0.0, true); }
RUDP_TEST(fault_burst_len8) { run_row("burst8", 0.20, 8, 0, 0, 0, 0.0, true); }
RUDP_TEST(fault_delay_jitter) {
  run_row("delay", 0.01, 0, ns_from_ms(3), 0, 0, 0.0, true);
}
RUDP_TEST(fault_reorder) { run_row("reorder", 0.02, 0, 0, 0.10, 0, 0.0, true); }
RUDP_TEST(fault_duplicate) {
  run_row("dup", 0.02, 0, 0, 0, 0.05, 0.0, true);
}
RUDP_TEST(fault_asymmetric) {
  // 10% downlink loss, clean uplink.
  SimSession s(1, 0x5EED);
  DirectionFaults down;
  down.data_loss = 0.10;
  s.net.set_route(s.gw_sock.local(), down);
  DirectionFaults up;
  up.base_delay = ns_from_ms(2);
  s.net.set_route(s.clients[0].ep, up);
  s.run(ns_from_ms(2500));
  s.shutdown_close();
  s.assert_invariants();
}
RUDP_TEST(fault_control_loss) {
  // 5% loss on ACK/NACK path exercises RTO-driven liveness.
  run_row("ctrl", 0.0, 0, 0, 0, 0, 0.05, false);
}
RUDP_TEST(fault_mixed_storm) {
  // Everything at once: the "storm" row.
  run_row("storm", 0.10, 4, ns_from_ms(2), 0.05, 0.03, 0.03, false);
}

}  // namespace rudp_test

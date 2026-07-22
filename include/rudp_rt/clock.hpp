#pragma once
// rudp_rt/clock.hpp — injected monotonic time (v0.2 D-30).
//
// IClock::now() returns MonoNs and never moves backward. RealClock uses
// CLOCK_MONOTONIC. VirtualClock is the simulator authority: tests advance it
// explicitly, which makes every protocol decision reproducible (spec §0.1 #4).

#include "rudp_rt/types.hpp"

#include <cassert>
#include <ctime>

namespace rudp_rt {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual MonoNs now() const = 0;
};

class RealClock final : public IClock {
 public:
  MonoNs now() const override {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<MonoNs>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
  }
};

class VirtualClock final : public IClock {
 public:
  MonoNs now() const override { return now_ns_; }

  // Jump or step forward; time never moves backward.
  void set(MonoNs t) {
    assert(t >= now_ns_);
    now_ns_ = t;
  }
  void advance(MonoNs delta) {
    assert(delta >= 0);
    now_ns_ += delta;
  }

 private:
  MonoNs now_ns_ = 0;
};

}  // namespace rudp_rt

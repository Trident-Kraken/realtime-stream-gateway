#pragma once
// rudp_rt/media.hpp — media frame value type and frame source interfaces.
//
// MediaFrame owns its bytes; the source ledger records every created frame
// (spec §6). The default synthetic source is 30 fps, 12,000 media bytes per
// frame, frame ids from 0, RTP timestamps advancing by 3,000 at 90 kHz.

#include "rudp_rt/types.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace rudp_rt {

constexpr std::uint32_t kRtpClockRate = 90000;

enum class FrameType : std::uint8_t {
  KEY_FRAME = 0,
  DELTA_FRAME = 1,
};

struct MediaFrame {
  std::uint16_t stream_id = 0;
  std::uint32_t frame_id = 0;
  std::uint32_t rtp_timestamp = 0;  // 90 kHz
  MonoNs capture_time = 0;          // monotonic, deadline origin (ADR-0004)
  MonoNs deadline_budget = 0;       // D; deadline = capture_time + D
  std::vector<std::byte> bytes;     // opaque deterministic media bytes
  FrameType frame_type = FrameType::KEY_FRAME;
  std::uint32_t depends_on_frame = 0;
};

// A source produces frames with monotonically increasing capture_time.
// next(now) returns the next scheduled frame whose capture time is due.
class IFrameSource {
 public:
  virtual ~IFrameSource() = default;
  virtual std::optional<MediaFrame> next(MonoNs now) = 0;
  virtual void request_keyframe() {}
};

// Deterministic synthetic source (default; spec §2.3).
class SyntheticSource final : public IFrameSource {
 public:
  struct Params {
    std::uint16_t stream_id = 1;
    std::uint32_t ssrc = 0;              // informational; RTP layer uses it
    std::uint32_t media_bytes = 12000;   // per frame
    std::uint32_t fps = 30;
    std::uint32_t rtp_ts_increment = 3000;
    MonoNs start_time = 0;               // first capture_time
    MonoNs deadline_budget = ns_from_ms(100);
    std::uint32_t gop_size = 0;          // 0 = all KEY_FRAME; >0 = periodic GOP
  };

  explicit SyntheticSource(Params p) : params_(p) {
    frame_interval_ns_ = p.fps > 0 ? kNsPerSec / static_cast<MonoNs>(p.fps)
                                   : kNsPerSec;
  }

  void request_keyframe() override { force_keyframe_ = true; }

  std::optional<MediaFrame> next(MonoNs now) override {
    const MonoNs capture = params_.start_time + next_frame_ * frame_interval_ns_;
    if (capture > now) return std::nullopt;
    MediaFrame f;
    f.stream_id = params_.stream_id;
    f.frame_id = next_frame_;
    f.rtp_timestamp = next_frame_ * params_.rtp_ts_increment;
    f.capture_time = capture;
    f.deadline_budget = params_.deadline_budget;
    f.bytes.resize(params_.media_bytes);
    fill_pattern(f.bytes, next_frame_, params_.stream_id);

    if (params_.gop_size > 0) {
      if (force_keyframe_ || (next_frame_ % params_.gop_size == 0)) {
        f.frame_type = FrameType::KEY_FRAME;
        f.depends_on_frame = 0;
        last_keyframe_id_ = next_frame_;
        force_keyframe_ = false;
      } else {
        f.frame_type = FrameType::DELTA_FRAME;
        f.depends_on_frame = last_keyframe_id_;
      }
    } else {
      f.frame_type = FrameType::KEY_FRAME;
      f.depends_on_frame = 0;
    }

    ++next_frame_;
    return f;
  }

  // Kept for the capture-worker call sites that set the budget after
  // construction; the value is applied to all subsequently produced frames.
  void set_deadline_budget(MonoNs d) { params_.deadline_budget = d; }

  // Deterministic byte pattern: cheap, reproducible, position-sensitive.
  static void fill_pattern(std::vector<std::byte>& bytes, std::uint32_t frame_id,
                           std::uint16_t stream_id);

  std::uint32_t frames_produced() const { return next_frame_; }

 private:
  Params params_;
  MonoNs frame_interval_ns_ = 0;
  std::uint32_t next_frame_ = 0;
  std::uint32_t last_keyframe_id_ = 0;
  bool force_keyframe_ = false;
};

// Callback signature used by capture workers to publish frames.
using FrameSink = std::function<void(MediaFrame)>;

// Stretch deliverable (camera_source.cpp): opens a V4L2 device; returns
// nullptr on failure (the core gate never depends on the camera).
std::unique_ptr<IFrameSource> open_camera_source(const std::string& device,
                                                 std::uint16_t stream_id,
                                                 std::uint32_t fps,
                                                 std::string* error);

}  // namespace rudp_rt

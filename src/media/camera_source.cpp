// camera_source.cpp — optional V4L2 USB camera source (stretch deliverable).
//
// The camera is explicitly non-blocking for the core gate (spec §2.1):
// `open()` returns nullptr when no device is available and `next()` reports
// failure through the error string instead of throwing. The protocol path
// never depends on this file being functional.

#include "rudp_rt/media.hpp"
#include "rudp_rt/wire.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

namespace rudp_rt {

namespace {

// Minimal V4L2 reader: captures one raw frame per call at a fixed interval.
// Only the happy path needed for the final demo is implemented; format
// negotiation is fixed to the device default and failures are reported as
// capture drops, never as protocol errors.
class V4l2Source final : public IFrameSource {
 public:
  V4l2Source(std::string device, std::uint16_t stream_id, MonoNs interval_ns)
      : device_(std::move(device)), stream_id_(stream_id),
        interval_ns_(interval_ns) {}

  ~V4l2Source() override {
    if (fd_ >= 0) ::close(fd_);
  }

  bool open(std::string* error) {
    fd_ = ::open(device_.c_str(), O_RDONLY);
    if (fd_ < 0) {
      if (error) *error = std::string("open: ") + std::strerror(errno);
      return false;
    }
    v4l2_capability cap{};
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) != 0) {
      if (error) *error = std::string("VIDIOC_QUERYCAP: ") + std::strerror(errno);
      return false;
    }
    return true;
  }

  std::optional<MediaFrame> next(MonoNs now) override {
    if (fd_ < 0 || failed_) return std::nullopt;
    const MonoNs capture = start_time_ + static_cast<MonoNs>(next_frame_) * interval_ns_;
    if (capture > now) return std::nullopt;
    std::vector<std::byte> bytes(kMaxMediaFragment);
    const ssize_t n = ::read(fd_, bytes.data(), bytes.size());
    if (n <= 0) {
      failed_ = true;  // device error: capture path disabled, protocol unaffected
      return std::nullopt;
    }
    bytes.resize(static_cast<std::size_t>(n));
    MediaFrame f;
    f.stream_id = stream_id_;
    f.frame_id = next_frame_;
    f.rtp_timestamp = static_cast<std::uint32_t>(
        (static_cast<std::int64_t>(next_frame_) * kRtpClockRate * interval_ns_) /
        kNsPerSec);
    f.capture_time = capture;
    f.deadline_budget = ns_from_ms(100);
    f.bytes = std::move(bytes);
    ++next_frame_;
    return f;
  }

 private:
  std::string device_;
  std::uint16_t stream_id_;
  MonoNs interval_ns_;
  MonoNs start_time_ = 0;
  std::uint32_t next_frame_ = 0;
  int fd_ = -1;
  bool failed_ = false;
};

}  // namespace

// Factory used by the runtime when a camera is explicitly requested.
// Returns nullptr when the device cannot be opened; the caller must fall
// back to the synthetic source (camera failure must not fail the core gate).
std::unique_ptr<IFrameSource> open_camera_source(const std::string& device,
                                                 std::uint16_t stream_id,
                                                 std::uint32_t fps,
                                                 std::string* error) {
  auto source = std::make_unique<V4l2Source>(
      device, stream_id, fps > 0 ? kNsPerSec / static_cast<MonoNs>(fps) : kNsPerSec / 30);
  if (!source->open(error)) return nullptr;
  return source;
}

}  // namespace rudp_rt

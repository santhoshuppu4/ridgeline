#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ridgeline/frame.h"
#include "ridgeline/time.h"

namespace ridgeline {

// Stands in for real capture (VideoFileFrameSource, which needs OpenCV) so the
// pipeline wiring can be tested with no video file and no OpenCV dependency.
//
// Next() fills a Frame IN PLACE — designed to be called from inside
// SpscRingBuffer::TryPushWith, so the synthetic pixels are written straight
// into the ring slot with no intermediate copy. (An earlier version filled a
// thread_local scratch vector and then copied it in; unnecessary once the
// ring buffer grew a zero-copy API.)
//
// Pixels are a solid gray level that changes with frame_index. Nothing
// downstream of this source inspects pixel content (FakeDetector keys off
// frame_index), but the bytes are valid BGR24 so a real detector could run on
// them without crashing.
class SyntheticFrameSource {
 public:
  explicit SyntheticFrameSource(std::uint32_t width = 64, std::uint32_t height = 64) : width_(width), height_(height) {}

  bool Next(Frame& out) {
    const std::size_t bytes = static_cast<std::size_t>(width_) * height_ * Frame::kBytesPerPixel;
    if (bytes > Frame::kMaxBytes) return false;
    std::memset(out.MutablePixels(), static_cast<int>(next_index_ % 256), bytes);
    out.used_bytes = bytes;
    out.width = width_;
    out.height = height_;
    out.format = PixelFormat::kBgr24;
    out.capture_time_unix_ns = NowUnixNs();
    out.frame_index = next_index_++;
    return true;
  }

 private:
  std::uint32_t width_;
  std::uint32_t height_;
  std::uint64_t next_index_ = 0;
};

}  // namespace ridgeline

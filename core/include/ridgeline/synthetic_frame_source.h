#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ridgeline/frame.h"
#include "ridgeline/time.h"

namespace ridgeline {

// STUDY NOTE: this stands in for real camera/video-file capture (OpenCV
// VideoCapture reading an .mp4, in Phase 1b-ii) so the rest of the pipeline
// — ring buffer, detector interface, K-of-N confirmation, gRPC emission —
// can be built and TESTED right now, without a video file, without OpenCV,
// and without the tests depending on real decoded pixel data none of them
// actually look at. Swapping this for real capture later changes exactly
// one component; nothing downstream needs to know the difference, which is
// the same "code against an interface" idea as detector.h.
//
// Generates a tiny valid Frame (a small solid-color block, not a real
// image — nothing downstream currently inspects pixel content) at a fixed
// resolution, tagged with a monotonic frame_index and a real capture
// timestamp so downstream latency measurements are meaningful even though
// the pixels themselves are synthetic.
class SyntheticFrameSource {
 public:
  explicit SyntheticFrameSource(std::uint32_t width = 64, std::uint32_t height = 64) : width_(width), height_(height) {}

  // Fills `out` with the next synthetic frame. Returns false only if the
  // frame is somehow too large for Frame's fixed storage (see frame.h) —
  // at 64x64 NV12 (~6KB) this will never happen in practice, but the
  // return value is checked rather than ignored so a future resolution
  // change that DOES exceed capacity fails loudly here instead of
  // silently corrupting whatever frame was already in the slot.
  bool Next(Frame& out) {
    const std::size_t bytes = static_cast<std::size_t>(width_) * height_ * 3 / 2;  // NV12
    thread_local std::vector<std::byte> scratch;
    scratch.assign(bytes, std::byte{static_cast<unsigned char>(next_index_ % 256)});
    const bool ok = out.CopyFrom(scratch.data(), scratch.size(), width_, height_, NowUnixNs(), next_index_);
    ++next_index_;
    return ok;
  }

 private:
  std::uint32_t width_;
  std::uint32_t height_;
  std::uint64_t next_index_ = 0;
};

}  // namespace ridgeline

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ridgeline {

// STUDY NOTE: this is what makes SpscRingBuffer<Frame, N> an arena instead of
// just "a lock-free queue." Frame has no pointer to heap-allocated pixel
// data — the pixel bytes live INSIDE the struct, in a fixed-size array. The
// ring buffer's slots are allocated once (see ring_buffer.h), and the capture
// thread writes pixels directly into a slot via TryPushWith, so the per-frame
// path never calls `new`.
//
// PIXEL FORMAT (changed in Phase 1b-ii): Phase 1a sized this for NV12, a
// common raw camera format. Phase 1b-ii decodes video with OpenCV, which
// produces BGR, and YOLOX's official preprocessing also expects BGR. Storing
// BGR24 directly avoids an NV12<->BGR conversion on every frame. If you later
// capture from a real camera that delivers NV12/YUYV, convert once at the
// capture boundary, not deep in the pipeline.
//
// The tradeoff: kMaxBytes must be an upper bound on any frame you'll capture,
// and every slot pays that memory cost (1280x720x3 = ~2.6MB) whether or not
// it's full. That's why ring capacity for Frames should be SMALL — a handful
// of frames, per ADR-0003 — and why Frames should never be declared as stack
// locals (use std::make_unique<Frame>() or work in-place in a ring slot).
enum class PixelFormat : std::uint8_t {
  kUnknown = 0,
  kBgr24 = 1,  // 3 bytes per pixel, row-major, B then G then R.
};

struct Frame {
  static constexpr std::size_t kMaxWidth = 1280;
  static constexpr std::size_t kMaxHeight = 720;
  static constexpr std::size_t kBytesPerPixel = 3;  // BGR24
  static constexpr std::size_t kMaxBytes = kMaxWidth * kMaxHeight * kBytesPerPixel;

  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t used_bytes = 0;
  PixelFormat format = PixelFormat::kUnknown;
  std::int64_t capture_time_unix_ns = 0;
  std::uint64_t frame_index = 0;  // Monotonic counter from the capture source; not the same as DetectionEvent::seq.

  std::array<std::byte, kMaxBytes> data{};

  // Copies `len` bytes from `src` into this frame's owned storage. Returns
  // false (and leaves the frame unchanged) if `len` exceeds capacity, rather
  // than truncating silently — a truncated video frame is worse than a
  // dropped one, since it can look like a valid, smaller image.
  bool CopyFrom(const std::byte* src, std::size_t len, std::uint32_t w, std::uint32_t h, std::int64_t capture_ns,
                std::uint64_t index, PixelFormat fmt = PixelFormat::kBgr24) {
    if (len > kMaxBytes) return false;
    std::memcpy(data.data(), src, len);
    used_bytes = len;
    width = w;
    height = h;
    format = fmt;
    capture_time_unix_ns = capture_ns;
    frame_index = index;
    return true;
  }

  const std::uint8_t* Pixels() const { return reinterpret_cast<const std::uint8_t*>(data.data()); }
  std::uint8_t* MutablePixels() { return reinterpret_cast<std::uint8_t*>(data.data()); }
};

}  // namespace ridgeline

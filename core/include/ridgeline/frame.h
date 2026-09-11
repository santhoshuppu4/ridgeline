#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ridgeline {

// STUDY NOTE: this is what makes SpscRingBuffer<Frame, N> an arena instead of
// just "a lock-free queue." Frame has no pointer to heap-allocated pixel
// data — the pixel bytes live INSIDE the struct, in a fixed-size array. That
// means the ring buffer's constructor, which default-constructs Capacity of
// these in place, is the only allocation that ever happens. Filling a frame
// on the capture thread is a memcpy into already-owned memory, not a `new`.
//
// The tradeoff: kMaxBytes must be an upper bound on any frame you'll capture
// (e.g. 1280x720 NV12), and every slot pays that memory cost whether or not
// it's holding a full-size frame. For a fixed camera resolution that's a
// reasonable, deliberate trade of memory for zero per-frame allocation.
// Document the actual number you pick and why in your own ADR.
struct Frame {
  // NV12 is the format ONNX Runtime / OpenCV pipelines commonly expect from
  // camera capture (Y plane full-res + interleaved U/V half-res). Sized for
  // 1280x720. Recompute for whatever your actual capture resolution is.
  static constexpr std::size_t kMaxWidth = 1280;
  static constexpr std::size_t kMaxHeight = 720;
  static constexpr std::size_t kMaxBytes = kMaxWidth * kMaxHeight * 3 / 2;  // NV12: 1.5 bytes/pixel

  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t used_bytes = 0;
  std::int64_t capture_time_unix_ns = 0;
  std::uint64_t frame_index = 0;  // Monotonic counter from the capture source; not the same as DetectionEvent::seq.

  std::array<std::byte, kMaxBytes> data{};

  // Copies `len` bytes from `src` into this frame's owned storage. Returns
  // false (and leaves the frame unchanged) if `len` exceeds capacity, rather
  // than truncating silently — a truncated video frame is worse than a
  // dropped one, since it can look like a valid, smaller image.
  bool CopyFrom(const std::byte* src, std::size_t len, std::uint32_t w, std::uint32_t h,
                std::int64_t capture_ns, std::uint64_t index) {
    if (len > kMaxBytes) return false;
    std::memcpy(data.data(), src, len);
    used_bytes = len;
    width = w;
    height = h;
    capture_time_unix_ns = capture_ns;
    frame_index = index;
    return true;
  }
};

}  // namespace ridgeline

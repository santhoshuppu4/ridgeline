#pragma once
#include <chrono>
#include <cstdint>

namespace ridgeline {
inline std::int64_t NowUnixNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace ridgeline

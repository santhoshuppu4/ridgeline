#pragma once
#include <chrono>
#include <cstdint>

namespace ridgeline {
struct BackoffPolicy {
  std::chrono::milliseconds base{200};
  std::chrono::milliseconds cap{30'000};
};
std::chrono::milliseconds BackoffCeiling(std::uint32_t attempt, const BackoffPolicy& policy);
std::chrono::milliseconds FullJitterBackoff(std::uint32_t attempt, const BackoffPolicy& policy, double u);
}  // namespace ridgeline

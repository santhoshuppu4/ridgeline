#include "ridgeline/backoff.h"
#include <algorithm>
#include <cmath>

namespace ridgeline {
std::chrono::milliseconds BackoffCeiling(std::uint32_t attempt, const BackoffPolicy& policy) {
  const double ceiling = static_cast<double>(policy.base.count()) * std::pow(2.0, static_cast<double>(attempt));
  if (ceiling >= static_cast<double>(policy.cap.count())) return policy.cap;
  return std::chrono::milliseconds{static_cast<std::int64_t>(ceiling)};
}
std::chrono::milliseconds FullJitterBackoff(std::uint32_t attempt, const BackoffPolicy& policy, double u) {
  const double clamped = std::clamp(u, 0.0, std::nextafter(1.0, 0.0));
  const auto ceiling = BackoffCeiling(attempt, policy);
  return std::chrono::milliseconds{static_cast<std::int64_t>(clamped * static_cast<double>(ceiling.count()))};
}
}  // namespace ridgeline

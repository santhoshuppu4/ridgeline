#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ridgeline::tools {

// Collects durations and reports nearest-rank percentiles. Exact, not an
// approximation: tool runs record at most tens of thousands of samples.
class LatencyStats {
 public:
  void Reserve(std::size_t n) { samples_ns_.reserve(n); }
  void Add(std::int64_t ns) { samples_ns_.push_back(ns); }
  std::size_t Count() const { return samples_ns_.size(); }

  // Returns milliseconds. Sorts a copy so Add() can keep being called.
  double PercentileMs(double p) const {
    if (samples_ns_.empty()) return 0.0;
    std::vector<std::int64_t> sorted = samples_ns_;
    std::sort(sorted.begin(), sorted.end());
    const auto idx = std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())));
    return static_cast<double>(sorted[idx]) / 1e6;
  }

  void Print(const char* label) const {
    std::printf("  %-28s n=%-6zu p50=%8.2f ms  p95=%8.2f ms  p99=%8.2f ms  max=%8.2f ms\n", label, Count(),
                PercentileMs(0.50), PercentileMs(0.95), PercentileMs(0.99), PercentileMs(1.0));
  }

 private:
  std::vector<std::int64_t> samples_ns_;
};

}  // namespace ridgeline::tools

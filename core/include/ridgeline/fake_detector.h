#pragma once

#include <cstdint>

#include "ridgeline/detector.h"

namespace ridgeline {

// A detector with no model behind it at all — used to test the
// capture -> ring buffer -> confirm -> emit pipeline in isolation from
// whatever real inference backend eventually gets plugged in.
//
// Fires positive for `burst_length` CONSECUTIVE frames out of every
// `period` frames -- e.g. burst_length=8, period=50 means frames
// [0,8), [50,58), [100,108), ... are positive, everything else negative.
// This models a real detection event (smoke visible across a run of
// consecutive frames), not an isolated single-frame blip -- which matters
// because K-of-N confirmation is a sliding window over consecutive frames:
// an earlier version of this class fired one isolated positive frame every
// 10 frames, which made 3-of-5 confirmation mathematically IMPOSSIBLE
// (no 5-frame window could ever contain 3 positives from events spaced 10
// apart). Caught by pipeline_test.cc actually running the combination and
// finding zero confirmations over 5000 frames -- worth reproducing
// yourself: this is exactly the kind of "the two pieces are each correct
// in isolation but incompatible together" bug that only an integration
// test catches, not either unit test alone.
class FakeDetector : public Detector {
 public:
  FakeDetector(std::uint64_t burst_length, std::uint64_t period) : burst_length_(burst_length), period_(period) {}

  DetectionResult Detect(const Frame& frame) override {
    DetectionResult r;
    r.positive = period_ > 0 && (frame.frame_index % period_) < burst_length_;
    r.confidence = r.positive ? 0.9f : 0.1f;
    if (r.positive) {
      r.x_min = 0.40f; r.y_min = 0.30f; r.x_max = 0.55f; r.y_max = 0.42f;
    }
    return r;
  }

 private:
  std::uint64_t burst_length_;
  std::uint64_t period_;
};

}  // namespace ridgeline

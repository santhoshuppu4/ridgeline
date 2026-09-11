#pragma once

#include "ridgeline/frame.h"

namespace ridgeline {

struct DetectionResult {
  bool positive = false;
  float confidence = 0.0f;
  // Normalized [0,1] box, matching proto/ridgeline/v1/ingest.proto's
  // BoundingBox. Only meaningful when positive == true.
  float x_min = 0, y_min = 0, x_max = 0, y_max = 0;
};

// STUDY NOTE: why this interface exists at all, separate from "just call
// ONNX Runtime directly in the consumer loop."
//
// The consumer loop (capture -> ring buffer -> ??? -> K-of-N -> emit) needs
// SOMETHING that turns a Frame into a DetectionResult, but it shouldn't need
// to know whether that something is a real model, a fake for testing, or
// (later) a different runtime entirely (TensorRT on a Jetson, say). Coding
// the consumer loop against this interface, not against OnnxRuntime's API
// directly, is what makes it possible to:
//   - Unit-test the pipeline (capture -> confirm -> emit) with a
//     deterministic FakeDetector, with no model file and no ONNX Runtime
//     dependency at all, which is exactly what tests/pipeline_test.cc does.
//   - Swap in a real detector later (OnnxCpuDetector, Phase 1b-ii) without
//     touching the pipeline code, only the one line that constructs it.
// This is the same "depend on an interface, not a concrete type" idea
// you'll recognize from any DI/testability discussion — applied here in
// plain C++ with no framework, just a pure virtual base class.
class Detector {
 public:
  virtual ~Detector() = default;
  virtual DetectionResult Detect(const Frame& frame) = 0;
};

}  // namespace ridgeline

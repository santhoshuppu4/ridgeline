#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ridgeline/frame.h"
#include "ridgeline/kofn_confirmer.h"
#include "ridgeline/onnx_cpu_detector.h"
#include "ridgeline/ring_buffer.h"
#include "ridgeline/time.h"
#include "ridgeline/video_file_frame_source.h"

namespace ridgeline {

// STUDY NOTE: this is tools/ridgeline_edge.cc's capture+inference logic,
// extracted into a reusable component so the SAME code path backs both the
// standalone diagnostic tool and the real agent -- rather than reimplementing
// the pipeline a second time in agent/src/main.cc and risking the two
// drifting apart. This is the same "one implementation, multiple callers"
// idea as Detector: ridgeline_edge printed confirmed events to stdout; the
// agent instead pushes them into a ring buffer for its gRPC send thread to
// drain. EdgePipeline itself doesn't know or care which.

struct ConfirmedEvent {
  std::uint64_t frame_index = 0;
  float confidence = 0.0f;
  float x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  std::int64_t capture_time_unix_ns = 0;
  std::int64_t decided_time_unix_ns = 0;
  std::uint32_t frames_confirmed = 0;  // Confirmer::PositiveCount() at the moment of confirmation.
  std::uint32_t window_size = 0;       // Confirmer::WindowSize() at the moment of confirmation.
};

struct EdgePipelineConfig {
  std::string video_path;
  std::string model_path;
  std::vector<int> target_class_ids;
  std::uint32_t k = 3;
  std::uint32_t n = 5;
  int intra_op_threads = 1;
  float score_threshold = 0.3f;
  bool realtime = true;  // false = decode as fast as possible, never drop (used by tests).
  bool loop = false;
};

// Runs capture and inference on two background threads, from construction
// until Stop() or destruction. Confirmed detections are pushed into
// `out_events` (owned by the caller, sized by the caller — matching
// ridgeline_edge's "the component doesn't own cross-thread state a caller
// might reasonably want visibility into" pattern already used for the frame
// ring buffer itself).
//
// Statistics (frames captured/dropped/processed, current frame-ring depth)
// are exposed via atomics so a caller can build a Heartbeat message from
// them without touching pipeline internals.
class EdgePipeline {
 public:
  static constexpr std::size_t kFrameRingSlots = 4;  // Small on purpose -- see ADR-0003.

  EdgePipeline(EdgePipelineConfig config, SpscRingBuffer<ConfirmedEvent, 256>& out_events);
  ~EdgePipeline();

  EdgePipeline(const EdgePipeline&) = delete;
  EdgePipeline& operator=(const EdgePipeline&) = delete;

  void Stop();  // Idempotent; also called by the destructor if not called explicitly.

  std::uint64_t FramesCaptured() const { return frames_captured_.load(std::memory_order_relaxed); }
  std::uint64_t FramesDropped() const { return frames_dropped_.load(std::memory_order_relaxed); }
  std::uint64_t FramesProcessed() const { return frames_processed_.load(std::memory_order_relaxed); }
  std::size_t FrameQueueDepth() const { return frame_ring_->SizeApprox(); }
  bool Finished() const { return finished_.load(std::memory_order_acquire); }  // True once source is exhausted (non-loop mode).

 private:
  void CaptureLoop();
  void InferenceLoop();

  EdgePipelineConfig config_;
  SpscRingBuffer<ConfirmedEvent, 256>& out_events_;

  VideoFileFrameSource source_;
  OnnxCpuDetector detector_;
  std::unique_ptr<SpscRingBuffer<Frame, kFrameRingSlots>> frame_ring_;
  std::unique_ptr<Frame> capture_scratch_;  // Decode target for frames dropped while the ring is full.

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> capture_done_{false};
  std::atomic<bool> finished_{false};
  std::atomic<std::uint64_t> frames_captured_{0};
  std::atomic<std::uint64_t> frames_dropped_{0};
  std::atomic<std::uint64_t> frames_processed_{0};

  std::thread capture_thread_;
  std::thread inference_thread_;
};

}  // namespace ridgeline

#include "ridgeline/edge_pipeline.h"

#include <chrono>

namespace ridgeline {

EdgePipeline::EdgePipeline(EdgePipelineConfig config, SpscRingBuffer<ConfirmedEvent, 256>& out_events)
    : config_(std::move(config)),
      out_events_(out_events),
      source_(config_.video_path, config_.loop),
      detector_([&] {
        OnnxDetectorConfig dc;
        dc.model_path = config_.model_path;
        dc.intra_op_threads = config_.intra_op_threads;
        dc.score_threshold = config_.score_threshold;
        dc.target_class_ids = config_.target_class_ids;
        return dc;
      }()),
      frame_ring_(std::make_unique<SpscRingBuffer<Frame, kFrameRingSlots>>()),
      capture_scratch_(std::make_unique<Frame>()) {
  capture_thread_ = std::thread(&EdgePipeline::CaptureLoop, this);
  inference_thread_ = std::thread(&EdgePipeline::InferenceLoop, this);
}

EdgePipeline::~EdgePipeline() { Stop(); }

void EdgePipeline::Stop() {
  const bool already = stop_requested_.exchange(true, std::memory_order_acq_rel);
  if (already) return;  // Idempotent: a second Stop() (or destructor after explicit Stop()) is a no-op.
  if (capture_thread_.joinable()) capture_thread_.join();
  if (inference_thread_.joinable()) inference_thread_.join();
}

void EdgePipeline::CaptureLoop() {
  const double fps = source_.Fps() > 0 ? source_.Fps() : 30.0;
  const auto period =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / fps));
  auto next_due = std::chrono::steady_clock::now();

  while (!stop_requested_.load(std::memory_order_relaxed)) {
    if (config_.realtime) {
      std::this_thread::sleep_until(next_due);
      next_due += period;
    }
    bool fill_ran = false;
    bool got_frame = false;
    const bool published = frame_ring_->TryPushWith([&](Frame& slot) {
      fill_ran = true;
      got_frame = source_.Next(slot);
      return got_frame;
    });
    if (published) {
      frames_captured_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (fill_ran && !got_frame) break;  // End of source.
    if (config_.realtime) {
      // Ring full: the source still "produced" this frame on schedule, so
      // decode and discard it rather than falling behind wall-clock time --
      // same drop-newest reasoning as ring_buffer.h's TryPush.
      if (!source_.Next(*capture_scratch_)) break;
      frames_dropped_.fetch_add(1, std::memory_order_relaxed);
    } else {
      std::this_thread::yield();  // Offline mode: wait for room, never drop.
    }
  }
  capture_done_.store(true, std::memory_order_release);
}

void EdgePipeline::InferenceLoop() {
  KOfNConfirmer confirmer(config_.k, config_.n);
  bool was_confirmed = false;

  auto process = [&](Frame& f) {
    const DetectionResult r = detector_.Detect(f);
    const bool now_confirmed = confirmer.Update(r.positive);
    frames_processed_.fetch_add(1, std::memory_order_relaxed);

    if (now_confirmed && !was_confirmed) {
      ConfirmedEvent ev;
      ev.frame_index = f.frame_index;
      ev.confidence = r.confidence;
      ev.x_min = r.x_min; ev.y_min = r.y_min; ev.x_max = r.x_max; ev.y_max = r.y_max;
      ev.capture_time_unix_ns = f.capture_time_unix_ns;
      ev.decided_time_unix_ns = NowUnixNs();
      ev.frames_confirmed = confirmer.PositiveCount();
      ev.window_size = static_cast<std::uint32_t>(confirmer.WindowSize());
      // Drop-newest if the OUTPUT ring is full too (e.g. the agent's send
      // thread is stalled on a dead gateway connection): a confirmed-event
      // ring backing up is a distinct failure mode from a frame ring backing
      // up, and deliberately not conflated with FramesDropped() above.
      out_events_.TryPush(ev);
    }
    was_confirmed = now_confirmed;
  };

  while (!capture_done_.load(std::memory_order_acquire) || frame_ring_->SizeApprox() > 0) {
    if (stop_requested_.load(std::memory_order_relaxed) && frame_ring_->SizeApprox() == 0) break;
    if (!frame_ring_->TryPopWith(process)) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
  finished_.store(true, std::memory_order_release);
}

}  // namespace ridgeline

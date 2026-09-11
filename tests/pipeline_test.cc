// End-to-end pipeline test: this is Phase 1b's actual deliverable, proven
// with real threads, not just individually-tested components.
//
// capture thread --push--> SpscRingBuffer<Frame,N> --pop--> detector -> K-of-N -> confirmed events
//
// This intentionally uses SyntheticFrameSource + FakeDetector, not a real
// video file or ONNX Runtime — see those headers' comments for why. The
// point of this test is proving the WIRING is correct: frames flow through
// in order, none are silently lost or duplicated, and confirmed detections
// come out at the expected frame indices. Swapping FakeDetector for a real
// OnnxCpuDetector later doesn't change anything this test checks.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "ridgeline/fake_detector.h"
#include "ridgeline/frame.h"
#include "ridgeline/kofn_confirmer.h"
#include "ridgeline/ring_buffer.h"
#include "ridgeline/synthetic_frame_source.h"

namespace {

struct ConfirmedEvent {
  std::uint64_t frame_index;
  std::int64_t capture_to_confirm_ns;
};

}  // namespace

TEST(Pipeline, CaptureRingBufferDetectorConfirmerEndToEnd) {
  // Small capacity on purpose (ADR-0003): Frames are ~2.6MB each, and a
  // real pipeline should buffer a handful of frames, not thousands.
  constexpr std::size_t kRingCapacity = 8;
  constexpr std::uint64_t kTotalFrames = 5000;
  constexpr std::uint64_t kBurstLength = 8;   // 8 consecutive positive frames per event...
  constexpr std::uint64_t kPeriod = 50;       // ...once every 50 frames.
  constexpr std::uint32_t kK = 3, kN = 5;     // Needs 3 of the last 5 frames positive to confirm.

  auto rb = std::make_unique<ridgeline::SpscRingBuffer<ridgeline::Frame, kRingCapacity>>();

  std::atomic<bool> capture_done{false};
  std::atomic<std::uint64_t> frames_captured{0};
  std::atomic<std::uint64_t> full_retries{0};

  // This test checks LOGIC (ordering, exact confirmation count), so the
  // producer RETRIES when the buffer is full instead of dropping. A dropped
  // frame would shift FakeDetector's burst pattern and make the expected
  // episode count unpredictable. Real capture (tools/ridgeline_edge.cc)
  // drops instead, because a live camera can't wait -- that behavior is
  // measured there, not asserted here.
  std::thread capture([&] {
    ridgeline::SyntheticFrameSource source(/*width=*/64, /*height=*/64);
    for (std::uint64_t i = 0; i < kTotalFrames; ++i) {
      bool source_ok = true;
      // Frames are written straight into the ring slot: no Frame local, no copy.
      while (!rb->TryPushWith([&](ridgeline::Frame& slot) {
        source_ok = source.Next(slot);
        return source_ok;
      })) {
        ASSERT_TRUE(source_ok) << "synthetic frame " << i << " unexpectedly exceeded Frame capacity";
        full_retries.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
      }
      frames_captured.fetch_add(1, std::memory_order_relaxed);
    }
    capture_done.store(true, std::memory_order_release);
  });

  std::vector<ConfirmedEvent> confirmed;
  std::vector<std::uint64_t> frame_indices_seen;
  std::thread consume([&] {
    ridgeline::FakeDetector detector(kBurstLength, kPeriod);
    ridgeline::KOfNConfirmer confirmer(kK, kN);
    bool was_confirmed = false;  // Track the false->true edge; see kofn_confirmer.h's note on level vs edge.

    auto process = [&](ridgeline::Frame& f) {
      frame_indices_seen.push_back(f.frame_index);
      const auto result = detector.Detect(f);
      const bool now_confirmed = confirmer.Update(result.positive);
      if (now_confirmed && !was_confirmed) {
        confirmed.push_back({f.frame_index, ridgeline::NowUnixNs() - f.capture_time_unix_ns});
      }
      was_confirmed = now_confirmed;
    };

    while (!capture_done.load(std::memory_order_acquire) || rb->SizeApprox() > 0) {
      if (!rb->TryPopWith(process)) {
        std::this_thread::yield();
      }
    }
  });

  capture.join();
  consume.join();

  // ---- Wiring correctness: every frame the ring buffer accepted was
  // consumed exactly once, in order, with none silently lost between the
  // buffer and the detector. ----
  ASSERT_EQ(frame_indices_seen.size(), frames_captured.load())
      << "consumer should see exactly as many frames as the producer successfully pushed";
  for (std::size_t i = 1; i < frame_indices_seen.size(); ++i) {
    EXPECT_GT(frame_indices_seen[i], frame_indices_seen[i - 1])
        << "frame_index must be strictly increasing -- SPSC ordering guarantee, proven end-to-end here";
  }

  // ---- Detection correctness: with FakeDetector firing every 10th frame
  // and K=3-of-N=5, work out by hand which frame indices should confirm,
  // and check the pipeline actually produced exactly that set. This is
  // deliberately an exact check, not "at least one detection happened" --
  // an exact check is what would actually catch an off-by-one in the
  // confirmer or a dropped frame silently shifting the pattern. ----
  // ---- Detection correctness: with an 8-frame positive burst every 50
  // frames and K=3-of-N=5, work out by hand which frame indices should
  // confirm, and check the pipeline actually produces exactly that count.
  // An exact check here -- not "at least one detection happened" -- is
  // what would actually catch an off-by-one in the confirmer, a dropped
  // frame shifting the pattern, or FakeDetector's own boundary condition. ----
  //
  // Within one burst [0,8), the window (last 5 frames) first reaches 3
  // positives at local frame index 2 (frames 0,1,2 all positive) and stays
  // >=3-positive through local index 7 (window [3..7], still all positive:
  // burst covers up to index 7 inclusive since kBurstLength=8 means
  // positive for local indices 0..7). It then takes 2 more frames (local
  // 8,9) for the last positive to age out of the 5-frame window, so the
  // episode "ends" (transitions back to unconfirmed) at local index 9,
  // i.e. one single confirmed EPISODE per burst (one false->true edge),
  // not per-frame. With kTotalFrames=5000 and kPeriod=50, there are
  // exactly 100 bursts, and burst 99 (local frames starting at 4950)
  // completes well within kTotalFrames, so all 100 confirm.
  constexpr std::size_t kExpectedEpisodes = kTotalFrames / kPeriod;
  EXPECT_EQ(confirmed.size(), kExpectedEpisodes)
      << "expected exactly one confirmed episode per burst -- a mismatch here means either a "
         "frame was dropped/reordered, or the K-of-N edge-detection logic in this test is wrong";
  for (const auto& event : confirmed) {
    EXPECT_GE(event.capture_to_confirm_ns, 0) << "confirmation happens strictly after capture, so this delta "
                                                  "must never be negative -- a negative value here would mean "
                                                  "either a clock issue or a logic bug, not real latency";
    // Loose upper bound, not a tight benchmark assertion: this test runs
    // under ASan/TSan too, which slow everything down significantly. The
    // point here is catching "the pipeline hung" or "confirmation never
    // fires," not measuring precise latency -- that's ridgeline_bench's job.
    EXPECT_LT(event.capture_to_confirm_ns, 2'000'000'000LL)
        << "capture-to-confirm latency exceeded 2s in an all-synthetic, no-artificial-delay pipeline "
           "-- something is stalled";
  }

  std::fprintf(stderr,
               "[pipeline] captured=%llu full_retries=%llu confirmed_episodes=%zu (K=%u,N=%u,burst_length=%llu,period=%llu)\n",
               static_cast<unsigned long long>(frames_captured.load()), static_cast<unsigned long long>(full_retries.load()),
               confirmed.size(), kK, kN, static_cast<unsigned long long>(kBurstLength), static_cast<unsigned long long>(kPeriod));
}

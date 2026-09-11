// End-to-end Phase 1b-ii test with REAL video decoding and REAL inference:
//
//   VideoFileFrameSource --TryPushWith--> SpscRingBuffer<Frame,4> --TryPopWith--> OnnxCpuDetector --> KOfNConfirmer
//
// The test writes its own small video: alternating runs of the dog image
// (dog visible) and plain gray frames (nothing visible). Because we control
// exactly which frames contain a dog, the number of confirmed detection
// episodes is known in advance and asserted exactly, same philosophy as
// tests/pipeline_test.cc.
//
// Why MJPG in an .avi: it's the codec OpenCV can write without optional
// encoder packages, so the test doesn't depend on H.264 being installed.

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "ridgeline/coco_classes.h"
#include "ridgeline/kofn_confirmer.h"
#include "ridgeline/onnx_cpu_detector.h"
#include "ridgeline/ring_buffer.h"
#include "ridgeline/video_file_frame_source.h"

namespace {

constexpr int kSegments = 3;           // dog segment + gray segment, repeated
constexpr int kFramesPerSegment = 12;  // long enough for K=3-of-N=5 to confirm, and to fully un-confirm

std::filesystem::path WriteTestVideo() {
  const cv::Mat dog = cv::imread(RIDGELINE_TEST_IMAGE, cv::IMREAD_COLOR);
  if (dog.empty()) return {};
  const cv::Mat gray(dog.size(), CV_8UC3, cv::Scalar(128, 128, 128));

  auto path = std::filesystem::temp_directory_path() / "ridgeline_video_pipeline_test.avi";
  cv::VideoWriter writer(path.string(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 10.0, dog.size());
  if (!writer.isOpened()) return {};
  for (int s = 0; s < kSegments; ++s) {
    for (int i = 0; i < kFramesPerSegment; ++i) writer.write(dog);
    for (int i = 0; i < kFramesPerSegment; ++i) writer.write(gray);
  }
  writer.release();
  return path;
}

}  // namespace

TEST(VideoPipeline, RealDecodeRealInferenceConfirmsExactlyOneEpisodePerDogSegment) {
  const auto video = WriteTestVideo();
  ASSERT_FALSE(video.empty()) << "could not write test video (missing test image or MJPG writer support)";

  ridgeline::VideoFileFrameSource source(video.string());
  ridgeline::OnnxDetectorConfig config;
  config.model_path = RIDGELINE_MODEL_PATH;
  config.intra_op_threads = 1;
  config.target_class_ids = {ridgeline::kCocoDog};
  ridgeline::OnnxCpuDetector detector(config);

  auto ring = std::make_unique<ridgeline::SpscRingBuffer<ridgeline::Frame, 4>>();
  std::atomic<bool> capture_done{false};
  std::uint64_t captured = 0;

  // Offline mode: retry instead of drop, so every frame is inferred and the
  // expected count is exact. ridgeline_edge's --realtime mode is where drops
  // are measured.
  std::thread capture([&] {
    for (;;) {
      bool fill_ran = false;
      bool got = false;
      const bool published = ring->TryPushWith([&](ridgeline::Frame& slot) {
        fill_ran = true;
        got = source.Next(slot);
        return got;
      });
      if (published) {
        ++captured;
        continue;
      }
      if (fill_ran && !got) break;  // End of video.
      std::this_thread::yield();
    }
    capture_done.store(true, std::memory_order_release);
  });

  std::vector<std::uint64_t> episode_start_frames;
  std::uint64_t processed = 0;
  std::thread inference([&] {
    ridgeline::KOfNConfirmer confirmer(3, 5);
    bool was_confirmed = false;
    auto process = [&](ridgeline::Frame& f) {
      const bool now = confirmer.Update(detector.Detect(f).positive);
      if (now && !was_confirmed) episode_start_frames.push_back(f.frame_index);
      was_confirmed = now;
      ++processed;
    };
    while (!capture_done.load(std::memory_order_acquire) || ring->SizeApprox() > 0) {
      if (!ring->TryPopWith(process)) std::this_thread::yield();
    }
  });

  capture.join();
  inference.join();
  std::filesystem::remove(video);

  const auto total = static_cast<std::uint64_t>(kSegments * 2 * kFramesPerSegment);
  EXPECT_EQ(captured, total) << "every frame written to the video should be decoded";
  EXPECT_EQ(processed, total) << "every decoded frame should reach the detector (offline mode never drops)";

  ASSERT_EQ(episode_start_frames.size(), static_cast<std::size_t>(kSegments))
      << "expected one confirmed episode per dog segment";
  // With K=3 of N=5 and a dog on every frame of the segment, confirmation
  // fires on the 3rd dog frame: local index 2 of each dog segment.
  for (int s = 0; s < kSegments; ++s) {
    EXPECT_EQ(episode_start_frames[static_cast<std::size_t>(s)],
              static_cast<std::uint64_t>(s * 2 * kFramesPerSegment + 2))
        << "episode " << s << " should confirm on the third consecutive dog frame";
  }
}

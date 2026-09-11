// ridgeline_infer_bench: measures ONNX Runtime inference latency ALONE, on one
// fixed image, with no video decoding, ring buffer, or threads involved. This
// isolates the single most expensive stage so its cost can be quoted precisely
// and used to size the frame ring buffer (ADR-0003).
//
// Usage:
//   ./build/tools/ridgeline_infer_bench --threads=1 --iterations=200
//   ./build/tools/ridgeline_infer_bench --threads=4 --iterations=200

#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "latency_stats.h"
#include "ridgeline/coco_classes.h"
#include "ridgeline/onnx_cpu_detector.h"

int main(int argc, char** argv) {
  std::string model = RIDGELINE_DEFAULT_MODEL;
  std::string image = RIDGELINE_DEFAULT_IMAGE;
  int threads = 1;
  int iterations = 200;
  int warmup = 10;
  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    auto val = [&](std::string_view key) { return a.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr; };
    if (auto v = val("--model=")) model = v;
    else if (auto v2 = val("--image=")) image = v2;
    else if (auto v3 = val("--threads=")) threads = std::atoi(v3);
    else if (auto v4 = val("--iterations=")) iterations = std::atoi(v4);
    else if (auto v5 = val("--warmup=")) warmup = std::atoi(v5);
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
  }

  const cv::Mat img = cv::imread(image, cv::IMREAD_COLOR);
  if (img.empty()) { std::fprintf(stderr, "could not read image %s\n", image.c_str()); return 1; }
  auto frame = std::make_unique<ridgeline::Frame>();  // ~2.6MB: never a stack local.
  if (!frame->CopyFrom(reinterpret_cast<const std::byte*>(img.data), img.total() * img.elemSize(),
                       static_cast<std::uint32_t>(img.cols), static_cast<std::uint32_t>(img.rows), 0, 0)) {
    std::fprintf(stderr, "image too large for Frame (max %zux%zu)\n", ridgeline::Frame::kMaxWidth, ridgeline::Frame::kMaxHeight);
    return 1;
  }

  ridgeline::OnnxDetectorConfig config;
  config.model_path = model;
  config.intra_op_threads = threads;
  ridgeline::OnnxCpuDetector detector(config);

  // Warmup: the first runs pay one-time costs (memory arena growth, kernel
  // selection) that don't represent steady state.
  for (int i = 0; i < warmup; ++i) detector.DetectAll(*frame);

  ridgeline::tools::LatencyStats stats;
  stats.Reserve(static_cast<std::size_t>(iterations));
  std::size_t last_count = 0;
  for (int i = 0; i < iterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    last_count = detector.DetectAll(*frame).size();
    stats.Add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }

  std::printf("ridgeline_infer_bench\n  model=%s\n  image=%s (%dx%d)\n  intra_op_threads=%d  hardware_threads=%u  iterations=%d\n",
              model.c_str(), image.c_str(), img.cols, img.rows, threads, std::thread::hardware_concurrency(), iterations);
  stats.Print("preprocess+infer+postprocess");
  std::printf("  detections on image: %zu\n", last_count);
  for (const auto& d : detector.DetectAll(*frame)) {
    std::printf("    %-12s score=%.2f box=(%.0f,%.0f)-(%.0f,%.0f)\n", std::string(ridgeline::CocoClassName(d.class_id)).c_str(),
                d.score, d.x1, d.y1, d.x2, d.y2);
  }
  return 0;
}

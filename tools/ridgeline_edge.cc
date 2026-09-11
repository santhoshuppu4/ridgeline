// ridgeline_edge: the real Phase 1b edge pipeline, end to end.
//
//   capture thread:   VideoFileFrameSource --TryPushWith--> SpscRingBuffer<Frame, 4>
//   inference thread: --TryPopWith--> OnnxCpuDetector --> KOfNConfirmer --> confirmed event
//
// It reports the numbers that matter for an edge device:
//   - inference latency per frame (the expensive stage)
//   - capture-to-decision latency: from "frame decoded" to "detector + K-of-N
//     finished on it", which includes time spent waiting in the ring buffer
//   - frames dropped because inference couldn't keep up (only meaningful with
//     --realtime, which paces capture like a live camera)
//
// This does not talk to the gateway yet. Wiring confirmed events into the
// agent's gRPC stream is Phase 1b-iii.
//
// REMINDER: the stock yolox_nano model detects COCO classes (person, car, dog,
// ...), not smoke. This measures real inference plumbing and latency only.
//
// Usage:
//   ./build/tools/ridgeline_edge --video=third_party/testdata/sample.mp4 --classes=0 --threads=1

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "latency_stats.h"
#include "ridgeline/coco_classes.h"
#include "ridgeline/frame.h"
#include "ridgeline/kofn_confirmer.h"
#include "ridgeline/onnx_cpu_detector.h"
#include "ridgeline/ring_buffer.h"
#include "ridgeline/time.h"
#include "ridgeline/video_file_frame_source.h"

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

// Per ADR-0003: a Frame ring should hold a few frames, not thousands. 4 slots
// (3 usable) at ~2.6MB each is ~10MB, and caps queueing delay at ~3 inference
// periods. Compile-time because SpscRingBuffer's capacity is a template
// parameter (that's what lets the modulo become a bitmask).
constexpr std::size_t kFrameRingSlots = 4;

struct Options {
  std::string video;
  std::string model = RIDGELINE_DEFAULT_MODEL;
  std::vector<int> classes;  // empty = any class
  std::uint32_t k = 3;
  std::uint32_t n = 5;
  int threads = 1;
  float score = 0.3f;
  bool realtime = true;
  bool loop = false;
  std::uint64_t max_frames = 0;  // 0 = until end of video
};

std::vector<int> ParseClassList(const char* s) {
  std::vector<int> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(std::atoi(item.c_str()));
  }
  return out;
}

Options ParseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    auto val = [&](std::string_view key) { return a.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr; };
    if (auto v = val("--video=")) o.video = v;
    else if (auto v2 = val("--model=")) o.model = v2;
    else if (auto v3 = val("--classes=")) o.classes = ParseClassList(v3);
    else if (auto v4 = val("--k=")) o.k = static_cast<std::uint32_t>(std::atoi(v4));
    else if (auto v5 = val("--n=")) o.n = static_cast<std::uint32_t>(std::atoi(v5));
    else if (auto v6 = val("--threads=")) o.threads = std::atoi(v6);
    else if (auto v7 = val("--score=")) o.score = static_cast<float>(std::atof(v7));
    else if (auto v8 = val("--max-frames=")) o.max_frames = std::strtoull(v8, nullptr, 10);
    else if (a == "--no-realtime") o.realtime = false;
    else if (a == "--loop") o.loop = true;
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); std::exit(2); }
  }
  if (o.video.empty()) {
    std::fprintf(stderr, "usage: ridgeline_edge --video=PATH [--model=PATH] [--classes=0,2] [--k=3 --n=5] "
                         "[--threads=1] [--score=0.3] [--no-realtime] [--loop] [--max-frames=N]\n");
    std::exit(2);
  }
  if (o.k == 0 || o.n == 0 || o.k > o.n) {
    std::fprintf(stderr, "--k must be between 1 and --n\n");
    std::exit(2);
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = ParseArgs(argc, argv);
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  ridgeline::VideoFileFrameSource source(opt.video, opt.loop);
  const double fps = source.Fps() > 0 ? source.Fps() : 30.0;

  ridgeline::OnnxDetectorConfig config;
  config.model_path = opt.model;
  config.intra_op_threads = opt.threads;
  config.score_threshold = opt.score;
  config.target_class_ids = opt.classes;
  ridgeline::OnnxCpuDetector detector(config);

  auto ring = std::make_unique<ridgeline::SpscRingBuffer<ridgeline::Frame, kFrameRingSlots>>();
  auto scratch = std::make_unique<ridgeline::Frame>();  // Decode target for frames dropped while the ring is full.

  std::atomic<bool> capture_done{false};
  std::atomic<std::uint64_t> captured{0};
  std::atomic<std::uint64_t> dropped{0};

  std::printf("ridgeline_edge: video=%s fps=%.1f realtime=%s ring_slots=%zu K=%u N=%u threads=%d classes=%s\n",
              opt.video.c_str(), fps, opt.realtime ? "yes" : "no", kFrameRingSlots, opt.k, opt.n, opt.threads,
              opt.classes.empty() ? "any" : "filtered");

  const auto wall_start = Clock::now();

  std::thread capture([&] {
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
    auto next_due = Clock::now();
    std::uint64_t produced = 0;
    while (!g_stop.load() && (opt.max_frames == 0 || produced < opt.max_frames)) {
      if (opt.realtime) {
        // A live camera delivers frames on its own schedule, whether or not
        // inference is ready. Pacing here is what makes drop counts real.
        std::this_thread::sleep_until(next_due);
        next_due += period;
      }
      bool fill_ran = false;
      bool got_frame = false;
      const bool published = ring->TryPushWith([&](ridgeline::Frame& slot) {
        fill_ran = true;
        got_frame = source.Next(slot);
        return got_frame;
      });
      if (published) {
        captured.fetch_add(1, std::memory_order_relaxed);
        ++produced;
        continue;
      }
      if (fill_ran && !got_frame) break;  // End of video.
      if (opt.realtime) {
        // Ring full: the camera still produced this frame, so decode it (to
        // advance the video) and throw it away. Drop-newest, per ADR on ring_buffer.h.
        if (!source.Next(*scratch)) break;
        dropped.fetch_add(1, std::memory_order_relaxed);
        ++produced;
      } else {
        std::this_thread::yield();  // Offline mode: wait for room, never drop.
      }
    }
    capture_done.store(true, std::memory_order_release);
  });

  ridgeline::tools::LatencyStats infer_stats;
  ridgeline::tools::LatencyStats capture_to_decision;
  infer_stats.Reserve(100000);
  capture_to_decision.Reserve(100000);
  std::uint64_t processed = 0;
  std::uint64_t episodes = 0;

  std::thread inference([&] {
    ridgeline::KOfNConfirmer confirmer(opt.k, opt.n);
    bool was_confirmed = false;
    auto process = [&](ridgeline::Frame& f) {
      const auto t0 = Clock::now();
      const ridgeline::DetectionResult r = detector.Detect(f);
      const auto t1 = Clock::now();
      const bool now_confirmed = confirmer.Update(r.positive);
      const std::int64_t decided_ns = ridgeline::NowUnixNs();

      infer_stats.Add(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      capture_to_decision.Add(decided_ns - f.capture_time_unix_ns);
      ++processed;

      if (now_confirmed && !was_confirmed) {
        ++episodes;
        std::printf("[event] frame=%llu confidence=%.2f box=(%.3f,%.3f)-(%.3f,%.3f) window=%u/%zu latency=%.1fms\n",
                    static_cast<unsigned long long>(f.frame_index), r.confidence, r.x_min, r.y_min, r.x_max, r.y_max,
                    confirmer.PositiveCount(), confirmer.WindowSize(),
                    static_cast<double>(decided_ns - f.capture_time_unix_ns) / 1e6);
      }
      was_confirmed = now_confirmed;
    };
    while (!capture_done.load(std::memory_order_acquire) || ring->SizeApprox() > 0) {
      if (!ring->TryPopWith(process)) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  capture.join();
  inference.join();
  const double wall_s = std::chrono::duration<double>(Clock::now() - wall_start).count();

  const std::uint64_t total = captured.load() + dropped.load();
  std::printf("\nSummary (%.1f s wall, hardware_threads=%u)\n", wall_s, std::thread::hardware_concurrency());
  std::printf("  frames from source:          %llu\n", static_cast<unsigned long long>(total));
  std::printf("  frames processed:            %llu (%.1f fps)\n", static_cast<unsigned long long>(processed),
              static_cast<double>(processed) / wall_s);
  std::printf("  frames dropped (ring full):  %llu (%.1f%%)\n", static_cast<unsigned long long>(dropped.load()),
              total ? 100.0 * static_cast<double>(dropped.load()) / static_cast<double>(total) : 0.0);
  std::printf("  confirmed episodes:          %llu\n", static_cast<unsigned long long>(episodes));
  infer_stats.Print("inference");
  capture_to_decision.Print("capture-to-decision");
  return 0;
}

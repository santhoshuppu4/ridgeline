// ridgeline_edge: standalone diagnostic tool wrapping EdgePipeline.
//
// This now just drives EdgePipeline and drains its output ring buffer,
// instead of owning capture/inference logic itself. See
// inference/include/ridgeline/edge_pipeline.h's top comment for why: the
// agent (agent/src/main.cc, when built with --video) uses the exact same
// EdgePipeline class, so this tool and the real agent share one
// implementation instead of two that could drift apart.
//
// REMINDER: the stock yolox_nano model detects COCO classes, not smoke.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "latency_stats.h"
#include "ridgeline/coco_classes.h"
#include "ridgeline/edge_pipeline.h"
#include "ridgeline/ring_buffer.h"

namespace {

struct Options {
  std::string video;
  std::string model = RIDGELINE_DEFAULT_MODEL;
  std::vector<int> classes;
  std::uint32_t k = 3, n = 5;
  int threads = 1;
  float score = 0.3f;
  bool realtime = true;
  bool loop = false;
};

std::vector<int> ParseClassList(const char* s) {
  std::vector<int> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) if (!item.empty()) out.push_back(std::atoi(item.c_str()));
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
    else if (a == "--no-realtime") o.realtime = false;
    else if (a == "--loop") o.loop = true;
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); std::exit(2); }
  }
  if (o.video.empty()) {
    std::fprintf(stderr, "usage: ridgeline_edge --video=PATH [--model=PATH] [--classes=0,2] [--k=3 --n=5] "
                         "[--threads=1] [--score=0.3] [--no-realtime] [--loop]\n");
    std::exit(2);
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = ParseArgs(argc, argv);

  ridgeline::EdgePipelineConfig config;
  config.video_path = opt.video;
  config.model_path = opt.model;
  config.target_class_ids = opt.classes;
  config.k = opt.k;
  config.n = opt.n;
  config.intra_op_threads = opt.threads;
  config.score_threshold = opt.score;
  config.realtime = opt.realtime;
  config.loop = opt.loop;

  ridgeline::SpscRingBuffer<ridgeline::ConfirmedEvent, 256> events;
  std::printf("ridgeline_edge: video=%s realtime=%s K=%u N=%u threads=%d classes=%s\n", opt.video.c_str(),
              opt.realtime ? "yes" : "no", opt.k, opt.n, opt.threads, opt.classes.empty() ? "any" : "filtered");

  ridgeline::tools::LatencyStats capture_to_decision;
  capture_to_decision.Reserve(100000);
  std::uint64_t episodes = 0;

  const auto wall_start = std::chrono::steady_clock::now();
  {
    ridgeline::EdgePipeline pipeline(config, events);
    ridgeline::ConfirmedEvent ev;
    // Drain loop: this thread's only job is pulling confirmed events out of
    // the ring buffer as they arrive, exactly the role the agent's gRPC send
    // thread will play in Phase 1b-iii.
    while (!pipeline.Finished() || events.SizeApprox() > 0) {
      if (events.TryPop(ev)) {
        ++episodes;
        capture_to_decision.Add(ev.decided_time_unix_ns - ev.capture_time_unix_ns);
        std::printf("[event] frame=%llu confidence=%.2f box=(%.3f,%.3f)-(%.3f,%.3f) window=%u/%u latency=%.1fms\n",
                    static_cast<unsigned long long>(ev.frame_index), ev.confidence, ev.x_min, ev.y_min, ev.x_max,
                    ev.y_max, ev.frames_confirmed, ev.window_size,
                    static_cast<double>(ev.decided_time_unix_ns - ev.capture_time_unix_ns) / 1e6);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    const auto total = pipeline.FramesCaptured() + pipeline.FramesDropped();
    std::printf("\nSummary (%.1f s wall, hardware_threads=%u)\n", wall_s, std::thread::hardware_concurrency());
    std::printf("  frames from source:          %llu\n", static_cast<unsigned long long>(total));
    std::printf("  frames processed:            %llu (%.1f fps)\n",
                static_cast<unsigned long long>(pipeline.FramesProcessed()), static_cast<double>(pipeline.FramesProcessed()) / wall_s);
    std::printf("  frames dropped (ring full):  %llu (%.1f%%)\n", static_cast<unsigned long long>(pipeline.FramesDropped()),
                total ? 100.0 * static_cast<double>(pipeline.FramesDropped()) / static_cast<double>(total) : 0.0);
    std::printf("  confirmed episodes:          %llu\n", static_cast<unsigned long long>(episodes));
    capture_to_decision.Print("capture-to-decision");
  }
  return 0;
}

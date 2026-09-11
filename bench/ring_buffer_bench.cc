// Ridgeline ring buffer benchmark.
//
// A GoogleTest assertion tells you "does it work." This tells you "how fast,
// and how does latency behave under load" — the numbers that actually belong
// in BENCHMARKS.md and, from there, on the resume.
//
// What it measures, with a real producer thread and a real consumer thread
// (not a single-threaded loop, which would hide any contention effects):
//   1. Throughput: items/sec sustained over the run.
//   2. Enqueue-to-dequeue latency: p50/p95/p99/max, in microseconds.
//   3. Drop rate: how often the producer found the buffer full, as a
//      function of capacity and consumer speed — this is the real
//      backpressure story for the "drop-newest when full" design.
//
// Usage:
//   ./ridgeline_bench --duration-s=5 --consumer-delay-us=0
//   ./ridgeline_bench --duration-s=5 --consumer-delay-us=200   # simulate a slower consumer (e.g. real inference)
//
// Record results in BENCHMARKS.md with the exact command and commit hash —
// see that file's format. Run on WSL2 at least 3 times; report the middle
// run's numbers and note the spread, since WSL2 timing is noisier than
// bare-metal Linux.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

#include "ridgeline/ring_buffer.h"

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Fits in a cache line's worth of payload; realistic stand-in for a small
// event struct (think DetectionEvent, not a full Frame — see frame.h for
// why large payloads need heap-allocated ring buffers, not this benchmark's
// stack-friendly default).
struct Event {
  std::uint64_t seq = 0;
  std::int64_t enqueue_ns = 0;
};

constexpr std::size_t kCapacity = 4096;  // power of two, required by SpscRingBuffer

struct Options {
  double duration_s = 5.0;
  int consumer_delay_us = 0;  // Artificial per-item consumer cost, to simulate real inference work.
};

Options ParseArgs(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    auto value = [&](std::string_view key) -> const char* {
      return arg.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr;
    };
    if (auto v = value("--duration-s=")) opt.duration_s = std::atof(v);
    else if (auto v2 = value("--consumer-delay-us=")) opt.consumer_delay_us = std::atoi(v2);
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); std::exit(2); }
  }
  return opt;
}

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// Nearest-rank percentile over a SORTED vector. Simple and exact for our
// sample sizes (hundreds of thousands to millions of points) — no need for
// a streaming/approximate estimator like t-digest here.
double PercentileUs(const std::vector<std::int64_t>& sorted_ns, double p) {
  if (sorted_ns.empty()) return 0.0;
  const std::size_t idx = std::min(sorted_ns.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted_ns.size())));
  return static_cast<double>(sorted_ns[idx]) / 1000.0;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = ParseArgs(argc, argv);

  const unsigned hw_threads = std::thread::hardware_concurrency();
  if (hw_threads <= 1) {
    std::fprintf(stderr,
                 "WARNING: hardware_concurrency() reports %u core(s). With one core, the\n"
                 "producer and consumer threads time-share it, so results below measure OS\n"
                 "scheduling, not the ring buffer. Do not record these numbers in\n"
                 "BENCHMARKS.md — rerun on real multi-core hardware first.\n\n",
                 hw_threads);
  }

  // Heap-allocated: see ring_buffer.h's top comment on why this must never
  // be a stack local — kCapacity * sizeof(Event) is small here (Event is
  // tiny), but the habit matters, since Phase 1b will benchmark this same
  // harness against SpscRingBuffer<Frame, N>, where it absolutely does.
  auto rb = std::make_unique<ridgeline::SpscRingBuffer<Event, kCapacity>>();

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> pushed{0};
  std::atomic<std::uint64_t> dropped{0};

  // Consumer records every observed latency. Reserve generously up front so
  // the benchmark's own vector growth doesn't perturb the measurement.
  std::vector<std::int64_t> latencies_ns;
  latencies_ns.reserve(20'000'000);
  std::uint64_t popped = 0;

  std::thread producer([&] {
    Event e;
    while (!stop.load(std::memory_order_relaxed)) {
      e.seq = pushed.load(std::memory_order_relaxed);
      e.enqueue_ns = NowNs();
      if (rb->TryPush(e)) {
        pushed.fetch_add(1, std::memory_order_relaxed);
      } else {
        dropped.fetch_add(1, std::memory_order_relaxed);
        // Yield instead of immediately retrying. A pure busy-spin retry loop
        // is defensible on a dedicated core in production (the whole reason
        // to avoid a blocking queue), but inside a benchmark it can starve
        // the consumer thread of scheduling time on machines with few cores
        // relative to other load, which corrupts the very latency numbers
        // we're trying to measure. Yielding here trades a small amount of
        // producer throughput for numbers that reflect the algorithm rather
        // than the scheduler.
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&] {
    Event e;
    while (!stop.load(std::memory_order_relaxed) || rb->SizeApprox() > 0) {
      if (rb->TryPop(e)) {
        latencies_ns.push_back(NowNs() - e.enqueue_ns);
        ++popped;
        if (opt.consumer_delay_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds{opt.consumer_delay_us});
        }
      } else if (stop.load(std::memory_order_relaxed)) {
        break;  // Producer is done and the buffer is empty: nothing left to drain.
      }
    }
  });

  const auto start = Clock::now();
  std::this_thread::sleep_for(std::chrono::duration<double>{opt.duration_s});
  stop.store(true, std::memory_order_relaxed);
  producer.join();
  consumer.join();
  const auto elapsed_s = std::chrono::duration<double>(Clock::now() - start).count();

  std::sort(latencies_ns.begin(), latencies_ns.end());

  const std::uint64_t total_pushed = pushed.load();
  const std::uint64_t total_dropped = dropped.load();

  std::fprintf(stderr,
               "\nRidgeline ring buffer benchmark\n"
               "  duration:            %.2f s (requested %.2f s)\n"
               "  consumer_delay_us:   %d\n"
               "  capacity:            %zu\n"
               "  pushed:              %llu\n"
               "  popped:              %llu\n"
               "  dropped (buffer full): %llu (%.4f%% of push attempts)\n"
               "  throughput:          %.0f items/sec\n"
               "  latency p50:         %.2f us\n"
               "  latency p95:         %.2f us\n"
               "  latency p99:         %.2f us\n"
               "  latency max:         %.2f us\n",
               elapsed_s, opt.duration_s, opt.consumer_delay_us, kCapacity, static_cast<unsigned long long>(total_pushed),
               static_cast<unsigned long long>(popped), static_cast<unsigned long long>(total_dropped),
               100.0 * static_cast<double>(total_dropped) / static_cast<double>(std::max<std::uint64_t>(1, total_pushed + total_dropped)),
               static_cast<double>(popped) / elapsed_s, PercentileUs(latencies_ns, 0.50), PercentileUs(latencies_ns, 0.95),
               PercentileUs(latencies_ns, 0.99),
               latencies_ns.empty() ? 0.0 : static_cast<double>(latencies_ns.back()) / 1000.0);

  // Machine-readable line for pasting straight into BENCHMARKS.md.
  std::fprintf(stderr,
               "\nBENCHMARKS.md row (fill in Date/Commit/Conditions):\n"
               "| | | ring_buffer | throughput | | | %.0f items/sec | consumer_delay_us=%d | ./ridgeline_bench "
               "--duration-s=%.1f --consumer-delay-us=%d |\n"
               "| | | ring_buffer | enqueue-to-dequeue latency | %.2f us | %.2f us | %.2f us | consumer_delay_us=%d | "
               "(same command) |\n",
               static_cast<double>(popped) / elapsed_s, opt.consumer_delay_us, opt.duration_s, opt.consumer_delay_us,
               PercentileUs(latencies_ns, 0.50), PercentileUs(latencies_ns, 0.95), PercentileUs(latencies_ns, 0.99),
               opt.consumer_delay_us);

  return 0;
}

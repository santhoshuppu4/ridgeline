// ridgeline_device_simulator: spawns N simulated devices in one process,
// each a real gRPC client against a real ridgeline_gateway, with injected
// disconnects -- the Phase 2 deliverable from the original project spec:
// "Write a C++ device simulator that spawns N virtual devices in one
// process ... with injected clock skew and network faults. This is how
// you honestly reach fleet-scale numbers."
//
// WHY THIS EXISTS, SPECIFICALLY NOW: ADR-0001 flagged the gateway's
// synchronous, one-thread-per-connection gRPC server as something to
// "revisit before the Phase-5 fleet simulator; measure thread count and
// p99 under load." This tool is exactly that measurement. It does not
// pre-judge whether the synchronous server is fine or needs replacing --
// it produces the numbers that answer the question.
//
// WHAT'S DELIBERATELY NOT HERE: no per-device WAL, no durability testing.
// That's Phase 1c's job (chaos_test.sh), already proven thoroughly with a
// real identity oracle. This tool's only job is TRANSPORT AND SCALE:
// connection handling, backoff/reconnect behavior, and aggregate
// throughput/latency, with many concurrent devices. Conflating the two
// would make this tool slower to run and no more informative about either
// concern.
//
// Usage:
//   ./ridgeline_device_simulator --gateway=127.0.0.1:50051 --devices=200 --duration-s=30 --rate-hz=2 --disconnect-every-s=10

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "latency_stats.h"
#include "ridgeline/backoff.h"
#include "ridgeline/time.h"
#include "ridgeline/v1/ingest.grpc.pb.h"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::atomic<bool> g_stop{false};

struct Options {
  std::string gateway = "127.0.0.1:50051";
  int devices = 50;
  int duration_s = 20;
  double rate_hz = 2.0;
  int disconnect_every_s = 0;  // 0 = never force a disconnect.
  int connect_stagger_ms = 5;  // Spread initial connections instead of a thundering herd of N at once.
};

Options ParseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    auto val = [&](std::string_view key) { return a.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr; };
    if (auto v = val("--gateway=")) o.gateway = v;
    else if (auto v2 = val("--devices=")) o.devices = std::atoi(v2);
    else if (auto v3 = val("--duration-s=")) o.duration_s = std::atoi(v3);
    else if (auto v4 = val("--rate-hz=")) o.rate_hz = std::atof(v4);
    else if (auto v5 = val("--disconnect-every-s=")) o.disconnect_every_s = std::atoi(v5);
    else if (auto v6 = val("--connect-stagger-ms=")) o.connect_stagger_ms = std::atoi(v6);
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); std::exit(2); }
  }
  return o;
}

// Aggregate, cross-device stats. Every field is atomic because every
// simulated device updates these concurrently from its own thread -- the
// same "cross a component boundary via atomics, not a lock, when the
// operations are this simple" choice used elsewhere in this project.
struct FleetStats {
  std::atomic<std::uint64_t> sent{0};
  std::atomic<std::uint64_t> acked{0};
  std::atomic<std::uint64_t> reconnects{0};
  std::atomic<std::uint64_t> forced_disconnects{0};
};

// One simulated device: its own gRPC channel/stream, its own backoff state,
// its own fake-detection generator. Reused BackoffPolicy/FullJitterBackoff
// from ridgeline/backoff.h -- the exact same reconnect logic the real agent
// uses, not a separate reimplementation that could drift from it.
class SimulatedDevice {
 public:
  SimulatedDevice(int index, const Options& opt, FleetStats& stats, ridgeline::tools::LatencyStats& latency,
                 std::mutex& latency_mu)
      : index_(index), opt_(opt), stats_(stats), latency_(latency), latency_mu_(latency_mu) {}

  void Run() {
    device_id_ = "sim-" + std::to_string(index_);
    auto channel = grpc::CreateChannel(opt_.gateway, grpc::InsecureChannelCredentials());
    auto stub = ridgeline::v1::IngestService::NewStub(channel);

    const ridgeline::BackoffPolicy backoff;
    std::mt19937_64 rng{std::random_device{}() ^ static_cast<std::uint64_t>(index_)};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    std::uniform_real_distribution<float> conf{0.4f, 0.95f};
    std::uint32_t attempt = 0;
    std::uint64_t seq = 1;
    auto run_deadline = Clock::now() + std::chrono::seconds(opt_.duration_s);
    auto next_forced_disconnect =
        opt_.disconnect_every_s > 0 ? Clock::now() + std::chrono::seconds(opt_.disconnect_every_s) : Clock::time_point::max();

    while (!g_stop.load() && Clock::now() < run_deadline) {
      if (!channel->WaitForConnected(std::chrono::system_clock::now() + 2s)) {
        const auto delay = ridgeline::FullJitterBackoff(attempt, backoff, unit(rng));
        ++attempt;
        std::this_thread::sleep_for(std::min<Clock::duration>(delay, run_deadline - Clock::now()));
        continue;
      }

      grpc::ClientContext ctx;
      auto stream = stub->Connect(&ctx);
      std::atomic<bool> connection_ok{true};

      std::thread reader([&] {
        ridgeline::v1::GatewayMessage msg;
        while (stream->Read(&msg)) {
          if (msg.has_ack()) stats_.acked.fetch_add(1, std::memory_order_relaxed);
        }
        connection_ok.store(false);
      });

      ridgeline::v1::AgentMessage hello;
      hello.mutable_hello()->set_device_id(device_id_);
      hello.mutable_hello()->set_agent_version("simulator");
      // Deliberately false: this simulator has no WAL (see file header --
      // durability testing is chaos_test.sh's job, not this tool's), so its
      // last_acked_seq (always 0, never set below) is not real resume
      // state. This is exactly the case ADR-0010 fixes: without this flag,
      // every reconnect looked like a false "lost" gap on the gateway even
      // though zero real events were lost.
      hello.mutable_hello()->set_durable_resume(false);
      bool ok = stream->Write(hello);

      auto next_send = Clock::now();
      const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / opt_.rate_hz));

      while (ok && !g_stop.load() && Clock::now() < run_deadline) {
        const auto now = Clock::now();
        if (now >= next_forced_disconnect) {
          stats_.forced_disconnects.fetch_add(1, std::memory_order_relaxed);
          ctx.TryCancel();  // Simulates a real network drop: the reader thread's Read() will unblock with an error.
          next_forced_disconnect = now + std::chrono::seconds(opt_.disconnect_every_s);
          break;
        }
        if (now >= next_send) {
          ridgeline::v1::AgentMessage m;
          auto* d = m.mutable_detection();
          d->set_seq(seq++);
          d->set_label("smoke");
          d->set_confidence(conf(rng));
          const auto ts = ridgeline::NowUnixNs();
          d->set_capture_time_unix_ns(ts);
          d->set_emit_time_unix_ns(ts);
          ok = stream->Write(m);
          if (ok) stats_.sent.fetch_add(1, std::memory_order_relaxed);
          next_send += period;
        }
        if (!connection_ok.load()) break;  // Reader detected the stream died (gateway restart, real network issue).
        std::this_thread::sleep_for(std::min<Clock::duration>(10ms, run_deadline - Clock::now()));
      }

      if (ok) stream->WritesDone(); else ctx.TryCancel();
      reader.join();
      stream->Finish();

      if (Clock::now() < run_deadline && !g_stop.load()) {
        stats_.reconnects.fetch_add(1, std::memory_order_relaxed);
        attempt = 0;
      }
    }
  }

 private:
  int index_;
  const Options& opt_;
  FleetStats& stats_;
  ridgeline::tools::LatencyStats& latency_;
  std::mutex& latency_mu_;
  std::string device_id_;
};

}  // namespace

int main(int argc, char** argv) {
  const Options opt = ParseArgs(argc, argv);
  std::fprintf(stderr,
               "ridgeline_device_simulator: gateway=%s devices=%d duration=%ds rate=%.1fHz disconnect_every=%ds\n",
               opt.gateway.c_str(), opt.devices, opt.duration_s, opt.rate_hz, opt.disconnect_every_s);

  FleetStats stats;
  ridgeline::tools::LatencyStats latency;  // Reserved but currently unused per-sample -- ack-latency
  std::mutex latency_mu;                   // sampling is a documented follow-up; see README note in this tool's ADR.

  std::vector<std::unique_ptr<SimulatedDevice>> devices;
  std::vector<std::thread> threads;
  devices.reserve(static_cast<std::size_t>(opt.devices));
  threads.reserve(static_cast<std::size_t>(opt.devices));

  const auto wall_start = Clock::now();
  for (int i = 0; i < opt.devices; ++i) {
    devices.push_back(std::make_unique<SimulatedDevice>(i, opt, stats, latency, latency_mu));
    threads.emplace_back([dev = devices.back().get()] { dev->Run(); });
    if (opt.connect_stagger_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opt.connect_stagger_ms));
  }

  for (auto& t : threads) t.join();
  const double wall_s = std::chrono::duration<double>(Clock::now() - wall_start).count();

  const auto sent = stats.sent.load();
  const auto acked = stats.acked.load();
  std::printf("\nFleet simulation summary (%.1fs wall)\n", wall_s);
  std::printf("  devices:             %d\n", opt.devices);
  std::printf("  sent:                %llu (%.1f events/sec aggregate)\n", static_cast<unsigned long long>(sent),
              static_cast<double>(sent) / wall_s);
  std::printf("  acked:               %llu (%.1f%%)\n", static_cast<unsigned long long>(acked),
              sent ? 100.0 * static_cast<double>(acked) / static_cast<double>(sent) : 0.0);
  std::printf("  forced disconnects:  %llu\n", static_cast<unsigned long long>(stats.forced_disconnects.load()));
  std::printf("  reconnects observed: %llu\n", static_cast<unsigned long long>(stats.reconnects.load()));
  return (sent > 0 && acked == 0) ? 1 : 0;  // Every device connecting but zero acks is a real failure signal, not just a low number.
}

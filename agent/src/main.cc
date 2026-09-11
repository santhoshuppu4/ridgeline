// Ridgeline edge agent — Phase 0 networking + Phase 1 ring buffer, not yet wired together.
//
// This still streams FAKE detections (no camera, no ring buffer use yet).
// Your next step: replace the fake-detection generator below with a real
// producer/consumer split using ridgeline::SpscRingBuffer<Frame, N> — a
// capture "thread" pushes Frames in, an "inference thread" pops them, runs
// K-of-N confirmation, and hands confirmed detections to this gRPC loop.

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include "ridgeline/backoff.h"
#include "ridgeline/time.h"
#include "ridgeline/v1/ingest.grpc.pb.h"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

struct Options {
  std::string gateway = "localhost:50051";
  std::string device_id = "cam-0001";
  double rate_hz = 5.0;
  int duration_s = 0;
};

Options ParseArgs(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    auto value = [&](std::string_view key) -> const char* {
      return arg.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr;
    };
    if (auto v = value("--gateway=")) opt.gateway = v;
    else if (auto v2 = value("--device-id=")) opt.device_id = v2;
    else if (auto v3 = value("--rate-hz=")) opt.rate_hz = std::atof(v3);
    else if (auto v4 = value("--duration-s=")) opt.duration_s = std::atoi(v4);
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); std::exit(2); }
  }
  if (opt.rate_hz <= 0.0) { std::fprintf(stderr, "--rate-hz must be > 0\n"); std::exit(2); }
  return opt;
}

void SleepInterruptibly(Clock::duration d) {
  const auto deadline = Clock::now() + d;
  while (!g_stop.load()) {
    const auto now = Clock::now();
    if (now >= deadline) return;
    std::this_thread::sleep_for(std::min<Clock::duration>(deadline - now, 50ms));
  }
}

constexpr auto kConnectTimeout = 2s;

bool WaitForConnection(grpc::Channel& channel, Clock::duration timeout) {
  const auto deadline = Clock::now() + timeout;
  while (!g_stop.load() && Clock::now() < deadline) {
    if (channel.WaitForConnected(std::chrono::system_clock::now() + 250ms)) return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = ParseArgs(argc, argv);
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  const auto started = Clock::now();
  auto time_up = [&] { return opt.duration_s > 0 && Clock::now() - started >= std::chrono::seconds{opt.duration_s}; };

  std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> unit{0.0, 1.0};
  std::uniform_real_distribution<float> conf{0.40f, 0.95f};

  const ridgeline::BackoffPolicy backoff;
  std::uint32_t attempt = 0;
  std::uint64_t next_seq = 1;
  std::atomic<std::uint64_t> last_acked{0};

  grpc::ChannelArguments channel_args;
  channel_args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, static_cast<int>(backoff.base.count()));
  channel_args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, static_cast<int>(backoff.base.count()));
  channel_args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, static_cast<int>(backoff.cap.count()));
  auto channel = grpc::CreateCustomChannel(opt.gateway, grpc::InsecureChannelCredentials(), channel_args);
  auto stub = ridgeline::v1::IngestService::NewStub(channel);

  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>{1.0 / opt.rate_hz});

  while (!g_stop.load() && !time_up()) {
    if (!WaitForConnection(*channel, kConnectTimeout)) {
      if (g_stop.load() || time_up()) break;
      const auto delay = ridgeline::FullJitterBackoff(attempt, backoff, unit(rng));
      ++attempt;
      std::fprintf(stderr, "[agent] gateway %s unreachable, retrying in %lld ms\n", opt.gateway.c_str(),
                   static_cast<long long>(delay.count()));
      SleepInterruptibly(delay);
      continue;
    }

    grpc::ClientContext ctx;
    auto stream = stub->Connect(&ctx);

    std::atomic<bool> got_ack{false};
    std::thread reader([&] {
      ridgeline::v1::GatewayMessage msg;
      while (stream->Read(&msg)) {
        if (msg.has_ack()) {
          const std::uint64_t acked = msg.ack().up_to_seq();
          std::uint64_t prev = last_acked.load();
          while (acked > prev && !last_acked.compare_exchange_weak(prev, acked)) {}
          got_ack.store(true);
        }
      }
    });

    ridgeline::v1::AgentMessage hello_msg;
    auto* hello = hello_msg.mutable_hello();
    hello->set_device_id(opt.device_id);
    hello->set_agent_version(RIDGELINE_VERSION);
    hello->set_last_acked_seq(last_acked.load());
    bool ok = stream->Write(hello_msg);

    auto next_detection = Clock::now();
    auto next_heartbeat = Clock::now();
    while (ok && !g_stop.load() && !time_up()) {
      const auto now = Clock::now();
      if (now >= next_detection) {
        ridgeline::v1::AgentMessage m;
        auto* d = m.mutable_detection();
        d->set_seq(next_seq);
        d->set_label("smoke");
        d->set_confidence(conf(rng));
        auto* box = d->mutable_bbox();
        box->set_x_min(0.40f); box->set_y_min(0.30f); box->set_x_max(0.55f); box->set_y_max(0.42f);
        const auto ts = ridgeline::NowUnixNs();
        d->set_capture_time_unix_ns(ts);
        d->set_emit_time_unix_ns(ts);
        ok = stream->Write(m);
        if (ok) ++next_seq;
        next_detection += period;
      }
      if (ok && now >= next_heartbeat) {
        ridgeline::v1::AgentMessage m;
        m.mutable_heartbeat()->set_sent_time_unix_ns(ridgeline::NowUnixNs());
        ok = stream->Write(m);
        next_heartbeat += 1s;
      }
      const auto wake = std::min(next_detection, next_heartbeat);
      if (wake > Clock::now()) std::this_thread::sleep_for(std::min<Clock::duration>(wake - Clock::now(), 50ms));
    }

    if (ok) stream->WritesDone(); else ctx.TryCancel();
    reader.join();
    const grpc::Status status = stream->Finish();

    const std::uint64_t unacked = (next_seq - 1) - std::min(last_acked.load(), next_seq - 1);
    if (g_stop.load() || time_up()) break;

    if (got_ack.load()) attempt = 0;
    const auto delay = ridgeline::FullJitterBackoff(attempt, backoff, unit(rng));
    ++attempt;
    std::fprintf(stderr, "[agent] stream ended (code=%d: %s), %llu unacked, retrying in %lld ms\n",
                 static_cast<int>(status.error_code()), status.error_message().c_str(),
                 static_cast<unsigned long long>(unacked), static_cast<long long>(delay.count()));
    SleepInterruptibly(delay);
  }

  const std::uint64_t sent = next_seq - 1;
  const std::uint64_t acked = last_acked.load();
  std::fprintf(stderr, "[agent] done: sent=%llu acked=%llu\n", static_cast<unsigned long long>(sent),
               static_cast<unsigned long long>(acked));
  if (opt.duration_s > 0 && (sent == 0 || acked != sent)) return 1;
  return 0;
}

// Ridgeline edge agent — Phase 1b-iii (real detection pipeline, optional) +
// Phase 1c (write-ahead log durability).
//
// TWO SOURCES OF DETECTIONS, same downstream handling either way:
//   --video=PATH   : real EdgePipeline (capture + ONNX inference + K-of-N),
//                    only available when built with -DRIDGELINE_WITH_ONNX=ON.
//   (no --video)   : synthetic fake-detection generator, same as Phase 0/1a.
//                    Kept so scripts/smoke_test.sh needs neither OpenCV nor
//                    ONNX Runtime -- it's testing gRPC transport + WAL
//                    durability, not the inference stack.
//
// DURABILITY (Phase 1c): every DetectionEvent is written to a WAL BEFORE
// being handed to the network. An in-memory `outbox` (all events not yet
// acked, in seq order) is what actually gets written to the gRPC stream --
// on startup it's pre-loaded from Wal::ReplayUnacked, and on every
// reconnect the whole outbox is resent from the front. That's what closes
// the gap Phase 0/1a had: a connection drop used to silently lose whatever
// was in flight; now a drop (or a full process kill -9) loses nothing that
// was successfully WAL-appended.
//
// See context/adr/0006-agent-write-ahead-log.md for the design rationale,
// including the deliberate simplifications (a mutex around `outbox`, rather
// than a lock-free structure -- outbox operations happen at event rate, not
// frame rate, so the hot-path lock-free discipline used for frame_ring
// elsewhere doesn't apply here).

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ridgeline/backoff.h"
#include "ridgeline/time.h"
#include "ridgeline/v1/ingest.grpc.pb.h"
#include "ridgeline/wal.h"

#ifdef RIDGELINE_HAVE_ONNX
#include "ridgeline/edge_pipeline.h"
#include "ridgeline/ring_buffer.h"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

struct Options {
  std::string gateway = "localhost:50051";
  std::string device_id = "cam-0001";
  double rate_hz = 5.0;          // Fake-detection mode only.
  int duration_s = 0;
  std::string state_dir;         // Default derived from device_id below.
  bool log_commits = false;      // Test oracle: print each event's identity once it is durably in the WAL.
  std::string tls_ca, tls_cert, tls_key;  // All three required together to enable mTLS; see ADR-0011.
#ifdef RIDGELINE_HAVE_ONNX
  std::string video;
  std::string model = RIDGELINE_DEFAULT_MODEL;
  std::vector<int> classes;
  std::uint32_t k = 3, n = 5;
  int threads = 1;
  float score = 0.3f;
  bool loop = false;
#endif
};

#ifdef RIDGELINE_HAVE_ONNX
std::vector<int> ParseClassList(const char* s) {
  std::vector<int> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) if (!item.empty()) out.push_back(std::atoi(item.c_str()));
  return out;
}
#endif

Options ParseArgs(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    auto value = [&](std::string_view key) -> const char* {
      return arg.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr;
    };
    if (auto v = value("--gateway=")) opt.gateway = v;
    else if (auto v2 = value("--device-id=")) opt.device_id = v2;
    else if (auto vca = value("--tls-ca=")) opt.tls_ca = vca;
    else if (auto vcert = value("--tls-cert=")) opt.tls_cert = vcert;
    else if (auto vkey = value("--tls-key=")) opt.tls_key = vkey;
    else if (auto v3 = value("--rate-hz=")) opt.rate_hz = std::atof(v3);
    else if (auto v4 = value("--duration-s=")) opt.duration_s = std::atoi(v4);
    else if (auto v5 = value("--state-dir=")) opt.state_dir = v5;
    else if (arg == "--log-commits") opt.log_commits = true;
#ifdef RIDGELINE_HAVE_ONNX
    else if (auto v6 = value("--video=")) opt.video = v6;
    else if (auto v7 = value("--model=")) opt.model = v7;
    else if (auto v8 = value("--classes=")) opt.classes = ParseClassList(v8);
    else if (auto v9 = value("--k=")) opt.k = static_cast<std::uint32_t>(std::atoi(v9));
    else if (auto v10 = value("--n=")) opt.n = static_cast<std::uint32_t>(std::atoi(v10));
    else if (auto v11 = value("--threads=")) opt.threads = std::atoi(v11);
    else if (auto v12 = value("--score=")) opt.score = static_cast<float>(std::atof(v12));
    else if (arg == "--loop") opt.loop = true;
#endif
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); std::exit(2); }
  }
  if (opt.rate_hz <= 0.0) { std::fprintf(stderr, "--rate-hz must be > 0\n"); std::exit(2); }
  if (opt.state_dir.empty()) opt.state_dir = "ridgeline-state/" + opt.device_id;
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

std::string ReadFileOrDie(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { std::fprintf(stderr, "[agent] cannot read %s\n", path.c_str()); std::exit(1); }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// All events not yet acked, in ascending seq order. Protected by a mutex --
// see the file-level comment on why this one piece of state isn't lock-free
// like frame_ring/events elsewhere in the pipeline.
class Outbox {
 public:
  void Push(std::uint64_t seq, std::string bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    items_.push_back({seq, std::move(bytes)});
  }
  void AckUpTo(std::uint64_t up_to_seq) {
    std::lock_guard<std::mutex> lock(mu_);
    while (!items_.empty() && items_.front().first <= up_to_seq) items_.pop_front();
  }
  // Snapshot for resending on (re)connect. Copies rather than holding the
  // lock across gRPC I/O, since Push()/AckUpTo() must never block on a
  // slow/stuck network write.
  std::vector<std::pair<std::uint64_t, std::string>> Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {items_.begin(), items_.end()};
  }
  std::size_t Size() const { std::lock_guard<std::mutex> lock(mu_); return items_.size(); }

 private:
  mutable std::mutex mu_;
  std::deque<std::pair<std::uint64_t, std::string>> items_;
};

}  // namespace

int main(int argc, char** argv) {
  const Options opt = ParseArgs(argc, argv);
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  std::error_code ec;
  fs::create_directories(opt.state_dir, ec);
  if (ec) { std::fprintf(stderr, "[agent] could not create state dir %s: %s\n", opt.state_dir.c_str(), ec.message().c_str()); return 1; }

  ridgeline::Wal wal((fs::path(opt.state_dir) / "events.wal").string(), (fs::path(opt.state_dir) / "events.ckpt").string());

  Outbox outbox;
  std::uint64_t next_seq = wal.LastAcked() + 1;
  {
    std::uint64_t max_replayed = wal.LastAcked();
    const std::size_t n = wal.ReplayUnacked([&](std::uint64_t seq, const std::string& bytes) {
      outbox.Push(seq, bytes);
      max_replayed = std::max(max_replayed, seq);
    });
    next_seq = max_replayed + 1;
    if (n > 0) std::fprintf(stderr, "[agent] replayed %zu unacked event(s) from WAL, resuming at seq=%llu\n", n,
                            static_cast<unsigned long long>(next_seq));
  }

  const auto started = Clock::now();
  auto time_up = [&] { return opt.duration_s > 0 && Clock::now() - started >= std::chrono::seconds{opt.duration_s}; };

  std::atomic<std::uint64_t> queue_depth{0};
  std::atomic<std::uint64_t> frames_dropped{0};

  // Config reconciliation (ADR-0012): the reader thread (below) writes a
  // newly-received ConfigUpdate here whenever the gateway pushes one; the
  // producer thread (fake-detection or --video) polls it and applies
  // changes, then advances applied_config_version, which the heartbeat
  // block reports back to the gateway -- closing the desired/reported loop.
  std::mutex config_mu;
  ridgeline::v1::ConfigUpdate pending_config;
  bool pending_config_set = false;
  std::atomic<std::uint64_t> applied_config_version{0};
  std::atomic<double> current_rate_hz{opt.rate_hz};       // Fake-detection mode reads this each cycle.
#ifdef RIDGELINE_HAVE_ONNX
  std::atomic<std::uint32_t> current_confirm_k{opt.k};    // --video mode reads these when (re)building EdgePipeline.
  std::atomic<std::uint32_t> current_confirm_n{opt.n};
  std::atomic<float> current_score_threshold{opt.score};
#endif

  std::thread producer;
  std::atomic<bool> producer_done{false};

  auto emit = [&](ridgeline::v1::DetectionEvent&& d) {
    d.set_seq(next_seq);
    const std::string bytes = d.SerializeAsString();
    wal.Append(next_seq, bytes);
    outbox.Push(next_seq, bytes);
    if (opt.log_commits) {
      // Printed only after Append() has fsync'd: from this line on, the event
      // is a promise. stderr is unbuffered, so the line survives kill -9.
      // scripts/chaos_test.sh checks every such promise against what the
      // gateway actually received, by (seq, capture_time) identity.
      std::fprintf(stderr, "[commit] seq=%llu capture_ns=%lld\n", static_cast<unsigned long long>(next_seq),
                   static_cast<long long>(d.capture_time_unix_ns()));
    }
    ++next_seq;
  };

#ifdef RIDGELINE_HAVE_ONNX
  if (!opt.video.empty()) {
    producer = std::thread([&] {
      bool config_changed = false;
      while (!g_stop.load() && !time_up()) {
        ridgeline::EdgePipelineConfig config;
        config.video_path = opt.video;
        config.model_path = opt.model;
        config.target_class_ids = opt.classes;
        config.k = current_confirm_k.load();
        config.n = current_confirm_n.load();
        config.intra_op_threads = opt.threads;
        config.score_threshold = current_score_threshold.load();
        config.realtime = true;
        config.loop = opt.loop;

        ridgeline::SpscRingBuffer<ridgeline::ConfirmedEvent, 256> events;
        ridgeline::EdgePipeline pipeline(config, events);
        ridgeline::ConfirmedEvent ev;
        config_changed = false;
        while (!g_stop.load() && !time_up() && (!pipeline.Finished() || events.SizeApprox() > 0)) {
          queue_depth.store(pipeline.FrameQueueDepth(), std::memory_order_relaxed);
          frames_dropped.store(pipeline.FramesDropped(), std::memory_order_relaxed);
          if (events.TryPop(ev)) {
            ridgeline::v1::DetectionEvent d;
            d.set_label("smoke");
            d.set_confidence(ev.confidence);
            auto* box = d.mutable_bbox();
            box->set_x_min(ev.x_min); box->set_y_min(ev.y_min); box->set_x_max(ev.x_max); box->set_y_max(ev.y_max);
            d.set_capture_time_unix_ns(ev.capture_time_unix_ns);
            d.set_emit_time_unix_ns(ridgeline::NowUnixNs());
            d.set_frames_confirmed(ev.frames_confirmed);
            d.set_window_size(ev.window_size);
            emit(std::move(d));
          } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }

          // Config reconciliation (ADR-0012): EdgePipeline's K-of-N and
          // score threshold are fixed at construction (see edge_pipeline.h),
          // so applying a new value means rebuilding the pipeline, not
          // mutating it in place. Breaking this inner loop lets `pipeline`
          // go out of scope (its destructor stops both internal threads
          // cleanly), and the outer loop reconstructs it with the new
          // atomics -- a real config-driven restart, not a no-op.
          std::lock_guard<std::mutex> lock(config_mu);
          if (pending_config_set) {
            const auto& cfg = pending_config;
            if (cfg.confirm_k() > 0) current_confirm_k.store(cfg.confirm_k());
            if (cfg.confirm_n() > 0) current_confirm_n.store(cfg.confirm_n());
            if (cfg.confidence_threshold() > 0) current_score_threshold.store(cfg.confidence_threshold());
            applied_config_version.store(cfg.version());
            std::fprintf(stderr,
                         "[agent] applied config version=%llu: confirm_k=%u confirm_n=%u score_threshold=%.2f "
                         "(rebuilding pipeline)\n",
                         static_cast<unsigned long long>(cfg.version()), current_confirm_k.load(),
                         current_confirm_n.load(), static_cast<double>(current_score_threshold.load()));
            pending_config_set = false;
            config_changed = true;
          }
          if (config_changed) break;
        }
        if (!config_changed) break;  // Pipeline genuinely finished (or g_stop/time_up fired), not a config-driven restart.
      }
      producer_done.store(true, std::memory_order_release);
    });
  }
#endif
  if (
#ifdef RIDGELINE_HAVE_ONNX
      opt.video.empty()
#else
      true
#endif
  ) {
    producer = std::thread([&] {
      std::mt19937_64 rng{std::random_device{}()};
      std::uniform_real_distribution<float> conf{0.40f, 0.95f};
      auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>{1.0 / current_rate_hz.load()});
      auto next_due = Clock::now();
      while (!g_stop.load() && !time_up()) {
        // Wait for the full period in short slices (so SIGINT/g_stop is
        // noticed promptly), THEN emit -- not "emit, then sleep up to 50ms
        // and loop regardless of whether the real period elapsed." The
        // latter is what this loop originally did, and it's a genuine bug:
        // capping every sleep at 50ms while incrementing next_due by the
        // FULL period each iteration means next_due drifts further ahead of
        // real time every single iteration, so the 50ms cap is what
        // actually governs the loop forever after the first couple of
        // iterations -- the configured rate_hz is silently ignored whenever
        // its period exceeds 50ms (rate_hz < 20). Invisible in this project
        // until now because every prior test used rate_hz >= 20 (smoke_test.sh,
        // chaos_test.sh, the fleet simulator's defaults); the mTLS test in
        // ADR-0011 was the first to use --rate-hz=5 --duration-s=2, and got
        // 40 events instead of the expected 10 -- exactly 2s / 50ms, not
        // 2s * 5Hz. Confirmed by reproducing with zero TLS involved.
        while (!g_stop.load() && !time_up() && Clock::now() < next_due) {
          std::this_thread::sleep_for(std::min<Clock::duration>(next_due - Clock::now(), 50ms));

          // Config reconciliation (ADR-0012): checked in this short-sleep
          // slice so a new target_fps takes effect within ~50ms of arriving,
          // not just at the top of the outer loop. Applying it here means
          // "adjust rate_hz" is a live parameter change, not a restart.
          std::lock_guard<std::mutex> lock(config_mu);
          if (pending_config_set) {
            const auto& cfg = pending_config;
            if (cfg.target_fps() > 0) {
              current_rate_hz.store(cfg.target_fps());
              period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>{1.0 / cfg.target_fps()});
              next_due = Clock::now();  // Re-baseline: don't apply a stale next_due computed under the old rate.
            }
            applied_config_version.store(cfg.version());
            std::fprintf(stderr, "[agent] applied config version=%llu: rate_hz=%.1f\n",
                         static_cast<unsigned long long>(cfg.version()), current_rate_hz.load());
            pending_config_set = false;
          }
        }
        if (g_stop.load() || time_up()) break;

        ridgeline::v1::DetectionEvent d;
        d.set_label("smoke");
        d.set_confidence(conf(rng));
        auto* box = d.mutable_bbox();
        box->set_x_min(0.40f); box->set_y_min(0.30f); box->set_x_max(0.55f); box->set_y_max(0.42f);
        const auto ts = ridgeline::NowUnixNs();
        d.set_capture_time_unix_ns(ts);
        d.set_emit_time_unix_ns(ts);
        emit(std::move(d));
        next_due += period;
      }
      producer_done.store(true, std::memory_order_release);
    });
  }

  const ridgeline::BackoffPolicy backoff;
  std::uint32_t attempt = 0;
  std::uint64_t bytes_since_compact = 0;

  grpc::ChannelArguments channel_args;
  channel_args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, static_cast<int>(backoff.base.count()));
  channel_args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, static_cast<int>(backoff.base.count()));
  channel_args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, static_cast<int>(backoff.cap.count()));
  auto channel = grpc::CreateCustomChannel(opt.gateway, [&]() -> std::shared_ptr<grpc::ChannelCredentials> {
    if (opt.tls_ca.empty() && opt.tls_cert.empty() && opt.tls_key.empty()) {
      return grpc::InsecureChannelCredentials();
    }
    if (opt.tls_ca.empty() || opt.tls_cert.empty() || opt.tls_key.empty()) {
      std::fprintf(stderr, "[agent] --tls-ca, --tls-cert, and --tls-key must all be provided together\n");
      std::exit(2);
    }
    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = ReadFileOrDie(opt.tls_ca);
    ssl_opts.pem_private_key = ReadFileOrDie(opt.tls_key);
    ssl_opts.pem_cert_chain = ReadFileOrDie(opt.tls_cert);
    std::fprintf(stderr, "[agent] mTLS enabled: presenting cert %s\n", opt.tls_cert.c_str());
    return grpc::SslCredentials(ssl_opts);
  }(), channel_args);
  auto stub = ridgeline::v1::IngestService::NewStub(channel);

  std::mt19937_64 jitter_rng{std::random_device{}()};
  std::uniform_real_distribution<double> unit{0.0, 1.0};

  auto producer_finished = [&] { return producer_done.load(std::memory_order_acquire); };
  // Exit once the producer has no more to give us (time limit reached in
  // fake mode, or the video source is exhausted in --video mode) AND
  // everything has been acked. An earlier version required time_up() AND
  // producer_finished() together, which meant --video mode without an
  // explicit --duration-s (the normal case: the agent should just process
  // the whole video and stop) never exited at all -- time_up() is always
  // false when duration_s == 0, so the exit condition could never become
  // true even after the video ended and every event was acked. Caught by
  // scripts/smoke_test_video.sh hanging until its own timeout wrapper
  // killed it, rather than by any assertion actually failing -- a reminder
  // that "the test hung" is itself a test result worth paying attention to.

  while (!g_stop.load() && !((time_up() || producer_finished()) && outbox.Size() == 0)) {
    if (!WaitForConnection(*channel, kConnectTimeout)) {
      if (g_stop.load()) break;
      const auto delay = ridgeline::FullJitterBackoff(attempt, backoff, unit(jitter_rng));
      ++attempt;
      std::fprintf(stderr, "[agent] gateway %s unreachable, retrying in %lld ms\n", opt.gateway.c_str(),
                   static_cast<long long>(delay.count()));
      SleepInterruptibly(delay);
      continue;
    }

    grpc::ClientContext ctx;
    auto stream = stub->Connect(&ctx);
    std::uint64_t last_sent_seq = 0;  // Reset per connection: outbox already holds only unacked items,
                                       // so a fresh connection always resends everything outstanding.
    std::atomic<bool> got_ack{false};
    std::thread reader([&] {
      ridgeline::v1::GatewayMessage msg;
      while (stream->Read(&msg)) {
        if (msg.has_ack()) {
          outbox.AckUpTo(msg.ack().up_to_seq());
          wal.Acknowledge(msg.ack().up_to_seq());
          got_ack.store(true);
        } else if (msg.has_config()) {
          std::lock_guard<std::mutex> lock(config_mu);
          if (msg.config().version() > pending_config.version() ||
              (!pending_config_set && applied_config_version.load() < msg.config().version())) {
            pending_config = msg.config();
            pending_config_set = true;
            std::fprintf(stderr, "[agent] received config update version=%llu (confirm_k=%u confirm_n=%u "
                                 "confidence_threshold=%.2f target_fps=%u)\n",
                         static_cast<unsigned long long>(msg.config().version()), msg.config().confirm_k(),
                         msg.config().confirm_n(), msg.config().confidence_threshold(), msg.config().target_fps());
          }
        }
      }
    });

    ridgeline::v1::AgentMessage hello_msg;
    auto* hello = hello_msg.mutable_hello();
    hello->set_device_id(opt.device_id);
    hello->set_agent_version(RIDGELINE_VERSION);
    hello->set_last_acked_seq(wal.LastAcked());
    hello->set_durable_resume(true);  // The WAL makes last_acked_seq trustworthy resume state; see ADR-0010.
    bool ok = stream->Write(hello_msg);

    if (ok) {
      for (const auto& kv : outbox.Snapshot()) {
        ridgeline::v1::AgentMessage m;
        m.mutable_detection()->ParseFromString(kv.second);
        ok = stream->Write(m);
        if (!ok) break;
        last_sent_seq = std::max(last_sent_seq, kv.first);
      }
    }

    auto next_heartbeat = Clock::now();
    while (ok && !g_stop.load() && !((time_up() || producer_finished()) && outbox.Size() == 0)) {
      const auto now = Clock::now();

      // Send anything with seq > last_sent_seq. Tracking by SEQ, not by
      // outbox index/size, is the point: acks concurrently pop_front old
      // entries out of the outbox while the producer appends new ones to
      // the back, so the deque's size shrinks and grows independently of
      // "how many items have been sent." An earlier version compared
      // outbox.Size() across passes and, whenever an ack popped entries
      // between passes, either skipped resending genuinely-new events or
      // resent already-sent ones out of the gateway's expected order --
      // the smoke test caught this directly as gateway-reported sequence
      // gaps (`lost=N` in its disconnect summary) even though the agent
      // itself believed everything was eventually acked. Seq numbers are
      // stable identities; indices into a mutating deque are not.
      for (const auto& kv : outbox.Snapshot()) {
        if (kv.first <= last_sent_seq) continue;
        ridgeline::v1::AgentMessage m;
        m.mutable_detection()->ParseFromString(kv.second);
        ok = stream->Write(m);
        if (!ok) break;
        last_sent_seq = std::max(last_sent_seq, kv.first);
      }

      if (ok && now >= next_heartbeat) {
        ridgeline::v1::AgentMessage m;
        auto* hb = m.mutable_heartbeat();
        hb->set_sent_time_unix_ns(ridgeline::NowUnixNs());
        hb->set_queue_depth(static_cast<std::uint32_t>(queue_depth.load(std::memory_order_relaxed)));
        hb->set_frames_dropped(frames_dropped.load(std::memory_order_relaxed));
        hb->set_applied_config_version(applied_config_version.load(std::memory_order_relaxed));
        ok = stream->Write(m);
        next_heartbeat += 1s;
      }
      if (!ok) break;
      std::this_thread::sleep_for(20ms);

      if (wal.FileSizeBytes() > bytes_since_compact + (1u << 20)) {
        wal.Compact();
        bytes_since_compact = wal.FileSizeBytes();
      }
    }

    if (ok) stream->WritesDone(); else ctx.TryCancel();
    reader.join();
    const grpc::Status status = stream->Finish();
    if (g_stop.load()) break;

    if (got_ack.load()) attempt = 0;
    const auto delay = ridgeline::FullJitterBackoff(attempt, backoff, unit(jitter_rng));
    ++attempt;
    std::fprintf(stderr, "[agent] stream ended (code=%d: %s), %zu unacked, retrying in %lld ms\n",
                 static_cast<int>(status.error_code()), status.error_message().c_str(), outbox.Size(),
                 static_cast<long long>(delay.count()));
    SleepInterruptibly(delay);
  }

  g_stop.store(true);
  if (producer.joinable()) producer.join();

  std::fprintf(stderr, "[agent] done: next_seq=%llu last_acked=%llu unacked=%zu\n",
               static_cast<unsigned long long>(next_seq - 1), static_cast<unsigned long long>(wal.LastAcked()), outbox.Size());
  if (opt.duration_s > 0 && outbox.Size() != 0) return 1;
  return 0;
}

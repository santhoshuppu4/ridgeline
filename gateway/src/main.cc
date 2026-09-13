// Ridgeline ingest gateway — Phase 0 transport + Phase 1d Kafka/Redis/DynamoDB.
//
// DURABILITY, MADE LITERAL: ADR-0001 said "an ack means durably stored" back
// when nothing backed that beyond the gateway process's own memory. With
// -DRIDGELINE_WITH_KAFKA=ON and --kafka-brokers set, the gateway publishes
// each validated DetectionEvent to Kafka and only sends its Ack once
// KafkaProducer::PublishSync() confirms the broker accepted it.
//
// Redis (hot state) and DynamoDB (device shadow) are both fed from
// Heartbeat messages, and both are explicitly BEST-EFFORT: a failure to
// update either never blocks or fails the detection ack path, since neither
// is on the durability-of-the-event critical path (see ADR-0008).
//
// With none of RIDGELINE_WITH_{KAFKA,REDIS,DYNAMODB} enabled, behavior is
// unchanged from Phase 1c: ack on validation, no external stores touched --
// this is what keeps scripts/smoke_test.sh and scripts/chaos_test.sh
// passing with nothing else running at all.

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/auth_context.h>
#include <grpc/grpc_security_constants.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include "ridgeline/time.h"
#include "ridgeline/token_bucket.h"
#include "ridgeline/v1/ingest.grpc.pb.h"

#ifdef RIDGELINE_HAVE_KAFKA
#include "ridgeline/kafka_producer.h"
#endif
#ifdef RIDGELINE_HAVE_REDIS
#include "ridgeline/redis_hot_state_store.h"
#endif
#ifdef RIDGELINE_HAVE_DYNAMODB
#include <nlohmann/json.hpp>
#include "ridgeline/curl_http_transport.h"
#include "ridgeline/device_shadow_store.h"
#endif
#ifdef RIDGELINE_HAVE_WEATHER
#include "ridgeline/alert_engine.h"
#include "ridgeline/curl_get_transport.h"
#include "ridgeline/weather_client.h"
#endif

namespace {
using namespace std::chrono_literals;
using ridgeline::v1::AgentMessage;
using ridgeline::v1::GatewayMessage;
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }
bool g_log_events = false;  // Test oracle output; see scripts/chaos_test.sh.

std::string ReadFileOrDie(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { std::fprintf(stderr, "[gateway] cannot read %s\n", path.c_str()); std::exit(1); }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#ifdef RIDGELINE_HAVE_WEATHER
// Background-refreshed weather snapshot (ADR-0014): a dedicated thread
// polls the real API every `refresh_s`, and Current() returns the most
// recent successful reading via a plain mutex -- called once per accepted
// detection, which is nowhere near hot enough to need anything fancier.
//
// ON A FAILED REFRESH: the PREVIOUS successful reading is kept, not
// cleared to nullopt -- a transient API hiccup shouldn't blank out weather
// context for every detection in between refreshes. This does mean a
// reading can go stale if the API stays down a long time; this phase does
// not track or enforce a max-age cutoff on that, which is a real,
// deliberate simplification worth revisiting if this ever needs to be
// trusted for longer outages.
class WeatherCache {
 public:
  WeatherCache(std::shared_ptr<ridgeline::WeatherClient> client, double lat, double lon, int refresh_s)
      : client_(std::move(client)), lat_(lat), lon_(lon), refresh_s_(std::max(refresh_s, 1)) {}
  ~WeatherCache() { Stop(); }
  WeatherCache(const WeatherCache&) = delete;
  WeatherCache& operator=(const WeatherCache&) = delete;

  void Start() {
    thread_ = std::thread([this] {
      while (!stop_.load()) {
        const auto result = client_->FetchCurrent(lat_, lon_);
        if (result.has_value()) {
          std::lock_guard<std::mutex> lock(mu_);
          current_ = result;
          std::fprintf(stderr, "[gateway] weather updated: temp=%.1fC humidity=%.0f%% wind=%.1fkm/h\n",
                       result->temperature_c, result->relative_humidity_pct, result->wind_speed_kmh);
        } else {
          std::fprintf(stderr, "[gateway] WARNING: weather fetch failed (keeping previous reading if any)\n");
        }
        for (int i = 0; i < refresh_s_ * 10 && !stop_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  void Stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }
  std::optional<ridgeline::WeatherConditions> Current() {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }

 private:
  std::shared_ptr<ridgeline::WeatherClient> client_;
  double lat_, lon_;
  int refresh_s_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mu_;
  std::optional<ridgeline::WeatherConditions> current_;
};
#endif

// Bundles every optional external integration into one struct so
// IngestServiceImpl's constructor stays a single, ordinary parameter list --
// no preprocessor gymnastics in the constructor itself, only in which
// fields this struct happens to have (each guarded individually, which is
// far more readable than trying to conditionally chain constructor
// initializer lists).
struct GatewayIntegrations {
#ifdef RIDGELINE_HAVE_KAFKA
  ridgeline::KafkaProducer* kafka = nullptr;
#endif
#ifdef RIDGELINE_HAVE_REDIS
  ridgeline::RedisHotStateStore* redis = nullptr;
#endif
#ifdef RIDGELINE_HAVE_DYNAMODB
  ridgeline::DeviceShadowStore* shadow_store = nullptr;
#endif
  std::string desired_configs_path;  // Empty = config reconciliation disabled. See ADR-0012.
#ifdef RIDGELINE_HAVE_WEATHER
  WeatherCache* weather_cache = nullptr;  // nullptr = weather fusion disabled. See ADR-0014.
#endif
  double rate_limit_capacity = 0;      // 0 = rate limiting disabled entirely. See ADR-0013.
  double rate_limit_tokens_per_second = 0;
};

// Parses the simple desired-config file format:
//   device_id,version,confirm_k,confirm_n,confidence_threshold,target_fps
// One device per line; blank lines and lines starting with '#' are
// skipped. Deliberately not JSON/YAML: this avoids pulling in a parsing
// dependency for a feature that has nothing to do with Kafka/Redis/
// DynamoDB and should work in every build configuration. Reloaded fresh on
// every call rather than cached -- correctness over micro-optimization at
// this scale; a real fleet-scale version would cache with an mtime check.
std::map<std::string, ridgeline::v1::ConfigUpdate> LoadDesiredConfigs(const std::string& path) {
  std::map<std::string, ridgeline::v1::ConfigUpdate> out;
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "[gateway] WARNING: could not read --device-configs=%s\n", path.c_str());
    return out;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string device_id, version_s, k_s, n_s, conf_s, fps_s;
    if (!std::getline(ss, device_id, ',') || !std::getline(ss, version_s, ',') || !std::getline(ss, k_s, ',') ||
        !std::getline(ss, n_s, ',') || !std::getline(ss, conf_s, ',') || !std::getline(ss, fps_s, ',')) {
      std::fprintf(stderr, "[gateway] WARNING: malformed line in %s, skipped: %s\n", path.c_str(), line.c_str());
      continue;
    }
    ridgeline::v1::ConfigUpdate cfg;
    cfg.set_version(std::strtoull(version_s.c_str(), nullptr, 10));
    cfg.set_confirm_k(static_cast<std::uint32_t>(std::strtoul(k_s.c_str(), nullptr, 10)));
    cfg.set_confirm_n(static_cast<std::uint32_t>(std::strtoul(n_s.c_str(), nullptr, 10)));
    cfg.set_confidence_threshold(static_cast<float>(std::atof(conf_s.c_str())));
    cfg.set_target_fps(static_cast<std::uint32_t>(std::strtoul(fps_s.c_str(), nullptr, 10)));
    out[device_id] = cfg;
  }
  return out;
}

#ifdef RIDGELINE_HAVE_DYNAMODB
// Read-modify-write with bounded retry on optimistic-concurrency conflict.
// A real use of the version machinery tested in isolation by
// tests/dynamodb/device_shadow_store_test.cc: two gateway instances (or two
// connections handled concurrently) writing the same device's shadow at
// once will have one succeed and one get kVersionConflict, retry by
// re-reading the new version, and succeed on the next attempt -- rather
// than silently overwriting each other's write.
void UpsertReportedWithRetry(ridgeline::DeviceShadowStore& store, const std::string& device_id,
                             const std::string& reported_json) {
  constexpr int kMaxAttempts = 3;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const auto current = store.GetReported(device_id);
    const std::int64_t expected_version = current ? current->version : 0;
    const auto result = store.PutReported(device_id, reported_json, expected_version);
    if (result == ridgeline::PutResult::kSuccess) return;
    if (result == ridgeline::PutResult::kError) return;  // Not a conflict -- e.g. DynamoDB unreachable; give up, don't spin.
    // kVersionConflict: someone else wrote first. Loop and retry with a freshly-read version.
  }
  std::fprintf(stderr, "[gateway] shadow update for %s gave up after %d version conflicts\n", device_id.c_str(),
               kMaxAttempts);
}
#endif

class IngestServiceImpl final : public ridgeline::v1::IngestService::Service {
 public:
  explicit IngestServiceImpl(GatewayIntegrations integrations) : integrations_(integrations) {}

  grpc::Status Connect(grpc::ServerContext* context, grpc::ServerReaderWriter<GatewayMessage, AgentMessage>* stream) override {
    std::string device_id;
    std::string tenant_id;  // Empty for a legacy/single-tenant device. See ADR-0013.
    std::uint64_t last_seq = 0, received = 0, duplicates = 0, gap_events = 0, rate_limited = 0;
    // See ADR-0010 / ingest.proto's Hello.durable_resume: true means
    // last_seq (seeded from Hello.last_acked_seq) is trustworthy resume
    // state, so a seq jump on the FIRST detection after Hello is a real
    // gap. false means the device told us up front it has no durable
    // memory -- the first detection after such a Hello resyncs last_seq to
    // whatever seq that detection carries, with no gap counted, since a
    // "gap" against state the device itself said was meaningless isn't a
    // real gap. Detections AFTER that resync are checked normally either
    // way: a real gap mid-stream is still a real gap.
    bool durable_resume = true;
    bool awaiting_resync = false;
#ifdef RIDGELINE_HAVE_KAFKA
    std::uint64_t kafka_published = 0, kafka_failed = 0;
#endif
    std::int64_t max_transit_ns = 0;
    AgentMessage msg;
    while (stream->Read(&msg)) {
      switch (msg.payload_case()) {
        case AgentMessage::kHello:
          if (msg.hello().device_id().empty()) return {grpc::StatusCode::INVALID_ARGUMENT, "hello.device_id is required"};
          // mTLS identity cross-check (ADR-0011, extended by ADR-0013 for
          // tenant identity): if the connection is authenticated via a
          // client certificate, its Common Name is a cryptographic fact --
          // the peer proved it holds that cert's private key. Certificates
          // minted by generate_test_certs.sh encode CN as
          // "tenant_id:device_id" when a tenant is issued, or just
          // "device_id" for a legacy/single-tenant cert. The expected CN
          // is constructed to match whichever shape the AGENT actually
          // claims, so a device with a single-tenant cert can't be tricked
          // into a tenant context it was never issued for, and a
          // tenant-scoped cert can't be presented while claiming a
          // DIFFERENT tenant_id than the one baked into it. On a plaintext
          // (non-mTLS) connection, FindPropertyValues returns empty and
          // this check is a no-op -- backward compatible with every
          // non-TLS test/script already in this project.
          {
            const auto cn_values = context->auth_context()->FindPropertyValues(GRPC_X509_CN_PROPERTY_NAME);
            if (!cn_values.empty()) {
              const std::string cert_cn(cn_values[0].data(), cn_values[0].size());
              const std::string expected_cn = msg.hello().tenant_id().empty()
                                                  ? msg.hello().device_id()
                                                  : msg.hello().tenant_id() + ":" + msg.hello().device_id();
              if (cert_cn != expected_cn) {
                std::fprintf(stderr, "[gateway] REJECTED: cert CN='%s' does not match claimed identity='%s'\n",
                             cert_cn.c_str(), expected_cn.c_str());
                return {grpc::StatusCode::PERMISSION_DENIED, "claimed identity does not match client certificate"};
              }
            }
          }
          device_id = msg.hello().device_id();
          tenant_id = msg.hello().tenant_id();
          last_seq = msg.hello().last_acked_seq();
          durable_resume = msg.hello().durable_resume();
          awaiting_resync = !durable_resume;
          std::fprintf(stderr, "[gateway] %s%s connected (resume after seq %llu)\n",
                       tenant_id.empty() ? "" : (tenant_id + ":").c_str(), device_id.c_str(),
                       static_cast<unsigned long long>(last_seq));
          break;
        case AgentMessage::kDetection: {
          if (device_id.empty()) return {grpc::StatusCode::FAILED_PRECONDITION, "hello must be first"};
          const auto& d = msg.detection();
          if (g_log_events) {
            std::fprintf(stderr, "[recv] device=%s seq=%llu capture_ns=%lld\n", device_id.c_str(),
                         static_cast<unsigned long long>(d.seq()), static_cast<long long>(d.capture_time_unix_ns()));
          }
          if (d.seq() <= last_seq) {
            ++duplicates;
          } else {
            const std::string rate_key = tenant_id.empty() ? ("device:" + device_id) : ("tenant:" + tenant_id);
            if (!CheckRateLimit(rate_key)) {
              ++rate_limited;
              break;  // No ack this round; agent's WAL-backed resend (ADR-0006) will retry once the bucket refills.
            }
#ifdef RIDGELINE_HAVE_KAFKA
            if (integrations_.kafka != nullptr) {
              if (integrations_.kafka->PublishSync(device_id, d.SerializeAsString())) {
                ++kafka_published;
              } else {
                ++kafka_failed;
                break;  // No ack this round; agent's WAL-backed resend (ADR-0006) will retry.
              }
            }
#endif
            if (awaiting_resync) {
              // First detection after a "no durable resume state" Hello:
              // accept whatever seq it carries as the new baseline, with no
              // gap penalty -- this is the ADR-0010 fix, verified by
              // scripts/gap_detection_test.sh to both suppress the false
              // gap here AND still catch a genuine mid-stream gap below.
              awaiting_resync = false;
            } else if (d.seq() != last_seq + 1) {
              gap_events += d.seq() - last_seq - 1;
            }
            last_seq = d.seq(); ++received;
            max_transit_ns = std::max(max_transit_ns, ridgeline::NowUnixNs() - d.emit_time_unix_ns());
#ifdef RIDGELINE_HAVE_WEATHER
            if (integrations_.weather_cache != nullptr) {
              const auto weather = integrations_.weather_cache->Current();
              const auto severity = ridgeline::ComputeAlertSeverity(d.confidence(), weather);
              if (severity != ridgeline::AlertSeverity::kNone) {
                std::fprintf(stderr, "[gateway] ALERT severity=%s device=%s confidence=%.2f%s\n",
                             ridgeline::ToString(severity).c_str(), device_id.c_str(),
                             static_cast<double>(d.confidence()), weather.has_value() ? "" : " (no weather data yet)");
              }
            }
#endif
          }
          GatewayMessage ack; ack.mutable_ack()->set_up_to_seq(last_seq);
          if (!stream->Write(ack)) return {grpc::StatusCode::UNAVAILABLE, "failed to write ack"};
          break;
        }
        case AgentMessage::kHeartbeat: {
          if (device_id.empty()) return {grpc::StatusCode::FAILED_PRECONDITION, "hello must be first"};
          const auto& hb = msg.heartbeat();
#ifdef RIDGELINE_HAVE_REDIS
          if (integrations_.redis != nullptr) {
            ridgeline::DeviceHotState hot;
            hot.last_seen_unix_ns = ridgeline::NowUnixNs();
            hot.queue_depth = hb.queue_depth();
            hot.frames_dropped = hb.frames_dropped();
            // Best-effort: a Redis hiccup must never affect the gRPC
            // stream or the detection-ack path (see file-level comment).
            integrations_.redis->Update(device_id, hot);
          }
#endif
#ifdef RIDGELINE_HAVE_DYNAMODB
          if (integrations_.shadow_store != nullptr) {
            nlohmann::json reported;
            reported["queue_depth"] = hb.queue_depth();
            reported["frames_dropped"] = hb.frames_dropped();
            reported["last_seen_unix_ns"] = ridgeline::NowUnixNs();
            UpsertReportedWithRetry(*integrations_.shadow_store, device_id, reported.dump());
          }
#endif
          // Config reconciliation (ADR-0012): compare the agent's reported
          // applied_config_version against desired state and push an
          // update if it's behind. Config, unlike Redis/DynamoDB above, has
          // no independent RIDGELINE_WITH_* flag -- it only needs core/proto
          // types, so it's always available; --device-configs simply
          // defaults to empty (disabled).
          if (!integrations_.desired_configs_path.empty()) {
            const auto desired = LoadDesiredConfigs(integrations_.desired_configs_path);
            const auto it = desired.find(device_id);
            if (it != desired.end() && it->second.version() > hb.applied_config_version()) {
              GatewayMessage gm;
              *gm.mutable_config() = it->second;
              if (!stream->Write(gm)) return {grpc::StatusCode::UNAVAILABLE, "failed to write config update"};
              std::fprintf(stderr, "[gateway] pushed config version=%llu to %s\n",
                           static_cast<unsigned long long>(it->second.version()), device_id.c_str());
            }
          }
          break;
        }
        case AgentMessage::PAYLOAD_NOT_SET:
          return {grpc::StatusCode::INVALID_ARGUMENT, "empty AgentMessage"};
      }
    }
    const std::string logged_identity =
        device_id.empty() ? "<no hello>" : (tenant_id.empty() ? device_id : tenant_id + ":" + device_id);
    // gap_events (raw) counts every skipped seq value between accepted
    // detections, from ANY cause -- including this connection's own
    // rate-limit rejections, which are a deliberate policy decision, not
    // data loss. Reporting the raw number as "lost" would repeat exactly
    // the mistake ADR-0010 fixed for reconnects (conflating an EXPLAINED
    // gap with a real one), just within a single stream instead of across
    // reconnects: a device throttled by its own tenant's rate limit would
    // look identical in this log to one that's actually losing events over
    // a bad link. Since every skipped seq in this connection is either a
    // rate-limit rejection or unexplained (no other cause exists within
    // one synchronous stream -- gRPC delivers what's written, in order),
    // subtracting the known rate_limited count isolates the genuinely
    // unexplained remainder. Clamped at zero: rate_limited can legitimately
    // exceed the raw gap sum (trailing rejections after the last accepted
    // detection never get tallied into gap_events at all, since nothing
    // arrives afterward to compute a jump against) -- that's not a bug,
    // just means those trailing skips are already fully accounted for by
    // rate_limited alone.
    const std::uint64_t attributed_lost = gap_events > rate_limited ? gap_events - rate_limited : 0;
    std::fprintf(stderr, "[gateway] %s disconnected: received=%llu duplicates=%llu lost=%llu rate_limited=%llu max_transit=%.2fms",
                 logged_identity.c_str(), static_cast<unsigned long long>(received),
                 static_cast<unsigned long long>(duplicates), static_cast<unsigned long long>(attributed_lost),
                 static_cast<unsigned long long>(rate_limited), static_cast<double>(max_transit_ns) / 1e6);
#ifdef RIDGELINE_HAVE_KAFKA
    if (integrations_.kafka != nullptr) {
      std::fprintf(stderr, " kafka_published=%llu kafka_failed=%llu", static_cast<unsigned long long>(kafka_published),
                   static_cast<unsigned long long>(kafka_failed));
    }
#endif
    std::fprintf(stderr, "\n");
    return grpc::Status::OK;
  }

 private:
  GatewayIntegrations integrations_;

  // Rate limiting (ADR-0013): one bucket per tenant (or per device_id, when
  // a device has no tenant_id -- see the keying comment where this is used
  // below). Shared across every concurrent connection this synchronous
  // server handles, so a mutex guards it -- rate-limit checks happen at
  // event rate (one per detection), not frame rate, so a plain mutex is
  // fine here, same reasoning as the Outbox class in agent/src/main.cc.
  std::mutex rate_limiter_mu_;
  std::map<std::string, ridgeline::TokenBucket> rate_limiters_;

  bool CheckRateLimit(const std::string& key) {
    if (integrations_.rate_limit_capacity <= 0) return true;  // Disabled.
    std::lock_guard<std::mutex> lock(rate_limiter_mu_);
    auto it = rate_limiters_.find(key);
    if (it == rate_limiters_.end()) {
      it = rate_limiters_
               .emplace(key, ridgeline::TokenBucket(integrations_.rate_limit_capacity,
                                                    integrations_.rate_limit_tokens_per_second, ridgeline::NowUnixNs()))
               .first;
    }
    return it->second.TryConsume(1.0, ridgeline::NowUnixNs());
  }
};
}  // namespace

int main(int argc, char** argv) {
  std::string listen = "0.0.0.0:50051";
  std::string tls_ca, tls_cert, tls_key;  // All three required together to enable mTLS; see ADR-0011.
  std::string device_configs_path;  // See ADR-0012.
#ifdef RIDGELINE_HAVE_WEATHER
  double weather_lat = 0.0, weather_lon = 0.0;
  bool weather_enabled = false;
  int weather_refresh_s = 300;  // 5 minutes: frequent enough to matter, far below any reasonable API rate limit.
#endif
  double rate_limit_capacity = 0;       // 0 = disabled. See ADR-0013.
  double rate_limit_tokens_per_second = 0;
#ifdef RIDGELINE_HAVE_KAFKA
  std::string kafka_brokers;
  std::string kafka_topic = "detections.v1";
  int kafka_timeout_ms = 5000;
#endif
#ifdef RIDGELINE_HAVE_REDIS
  std::string redis_host;
  int redis_port = 6379;
#endif
#ifdef RIDGELINE_HAVE_DYNAMODB
  std::string dynamodb_endpoint;
  std::string dynamodb_table = "device_shadows";
  std::string dynamodb_region = "us-west-2";
  std::string dynamodb_access_key = "local";
  std::string dynamodb_secret_key = "local";
#endif
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    [[maybe_unused]] auto value = [&](std::string_view key) -> const char* {
      return arg.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr;
    };
    if (arg.rfind("--listen=", 0) == 0) listen = std::string{arg.substr(9)};
    else if (arg == "--log-events") g_log_events = true;
    else if (auto vca = value("--tls-ca=")) tls_ca = vca;
    else if (auto vcert = value("--tls-cert=")) tls_cert = vcert;
    else if (auto vkey = value("--tls-key=")) tls_key = vkey;
    else if (auto vcfg = value("--device-configs=")) device_configs_path = vcfg;
#ifdef RIDGELINE_HAVE_WEATHER
    else if (auto vlat = value("--weather-lat=")) { weather_lat = std::atof(vlat); weather_enabled = true; }
    else if (auto vlon = value("--weather-lon=")) { weather_lon = std::atof(vlon); weather_enabled = true; }
    else if (auto vref = value("--weather-refresh-s=")) weather_refresh_s = std::atoi(vref);
#endif
    else if (auto vrlc = value("--rate-limit-capacity=")) rate_limit_capacity = std::atof(vrlc);
    else if (auto vrlr = value("--rate-limit-per-second=")) rate_limit_tokens_per_second = std::atof(vrlr);
#ifdef RIDGELINE_HAVE_KAFKA
    else if (auto v = value("--kafka-brokers=")) kafka_brokers = v;
    else if (auto v2 = value("--kafka-topic=")) kafka_topic = v2;
    else if (auto v3 = value("--kafka-timeout-ms=")) kafka_timeout_ms = std::atoi(v3);
#endif
#ifdef RIDGELINE_HAVE_REDIS
    else if (auto v4 = value("--redis-host=")) redis_host = v4;
    else if (auto v5 = value("--redis-port=")) redis_port = std::atoi(v5);
#endif
#ifdef RIDGELINE_HAVE_DYNAMODB
    else if (auto v6 = value("--dynamodb-endpoint=")) dynamodb_endpoint = v6;
    else if (auto v7 = value("--dynamodb-table=")) dynamodb_table = v7;
    else if (auto v8 = value("--dynamodb-region=")) dynamodb_region = v8;
    else if (auto v9 = value("--dynamodb-access-key=")) dynamodb_access_key = v9;
    else if (auto v10 = value("--dynamodb-secret-key=")) dynamodb_secret_key = v10;
#endif
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
  }
  std::signal(SIGINT, OnSignal); std::signal(SIGTERM, OnSignal);

  GatewayIntegrations integrations;

#ifdef RIDGELINE_HAVE_KAFKA
  std::unique_ptr<ridgeline::KafkaProducer> kafka;
  if (!kafka_brokers.empty()) {
    ridgeline::KafkaProducerConfig kcfg;
    kcfg.brokers = kafka_brokers;
    kcfg.topic = kafka_topic;
    kcfg.delivery_timeout_ms = kafka_timeout_ms;
    try {
      kafka = std::make_unique<ridgeline::KafkaProducer>(kcfg);
      integrations.kafka = kafka.get();
      std::fprintf(stderr, "[gateway] Kafka publish enabled: brokers=%s topic=%s\n", kafka_brokers.c_str(), kafka_topic.c_str());
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[gateway] failed to create Kafka producer: %s\n", e.what());
      return 1;
    }
  }
#endif
#ifdef RIDGELINE_HAVE_REDIS
  std::unique_ptr<ridgeline::RedisHotStateStore> redis;
  if (!redis_host.empty()) {
    ridgeline::RedisConfig rcfg;
    rcfg.host = redis_host;
    rcfg.port = redis_port;
    try {
      redis = std::make_unique<ridgeline::RedisHotStateStore>(rcfg);
      integrations.redis = redis.get();
      std::fprintf(stderr, "[gateway] Redis hot state enabled: %s:%d\n", redis_host.c_str(), redis_port);
    } catch (const std::exception& e) {
      // Deliberately NOT a fatal error, unlike a malformed Kafka/DynamoDB
      // config: Redis here is explicitly documented as best-effort,
      // ephemeral hot-state caching (see file header and ADR-0008), so a
      // Redis outage at gateway startup must not take down the whole
      // detection pipeline. Caught the inconsistency directly: an earlier
      // version of this code returned 1 here, matching Kafka/DynamoDB's
      // "fail loudly at construction" pattern -- but that pattern is right
      // for a CRITICAL dependency and wrong for a best-effort one. The
      // gateway now starts without Redis integration and logs a warning;
      // Update() calls elsewhere are already guarded by `integrations_.redis
      // != nullptr`, so nothing downstream needs to change.
      std::fprintf(stderr, "[gateway] WARNING: Redis unavailable at startup (%s) -- continuing without hot-state "
                          "caching. Detection ack path is unaffected.\n", e.what());
    }
  }
#endif
#ifdef RIDGELINE_HAVE_DYNAMODB
  std::shared_ptr<ridgeline::CurlHttpTransport> dynamodb_transport;
  std::unique_ptr<ridgeline::DeviceShadowStore> shadow_store;
  if (!dynamodb_endpoint.empty()) {
    ridgeline::DynamoDbConfig dcfg;
    dcfg.endpoint = dynamodb_endpoint;
    dcfg.table_name = dynamodb_table;
    dcfg.region = dynamodb_region;
    dcfg.access_key_id = dynamodb_access_key;
    dcfg.secret_access_key = dynamodb_secret_key;
    dynamodb_transport = std::make_shared<ridgeline::CurlHttpTransport>();
    shadow_store = std::make_unique<ridgeline::DeviceShadowStore>(dcfg, dynamodb_transport);
    integrations.shadow_store = shadow_store.get();
    std::fprintf(stderr, "[gateway] DynamoDB shadow enabled: endpoint=%s table=%s\n", dynamodb_endpoint.c_str(),
                 dynamodb_table.c_str());
  }
#endif

  integrations.desired_configs_path = device_configs_path;
#ifdef RIDGELINE_HAVE_WEATHER
  std::unique_ptr<WeatherCache> weather_cache;
  if (weather_enabled) {
    auto transport = std::make_shared<ridgeline::CurlGetTransport>();
    auto client = std::make_shared<ridgeline::WeatherClient>(transport);
    weather_cache = std::make_unique<WeatherCache>(client, weather_lat, weather_lon, weather_refresh_s);
    weather_cache->Start();
    integrations.weather_cache = weather_cache.get();
    std::fprintf(stderr, "[gateway] weather fusion enabled: lat=%.4f lon=%.4f refresh=%ds\n", weather_lat, weather_lon,
                 weather_refresh_s);
  }
#endif
  integrations.rate_limit_capacity = rate_limit_capacity;
  integrations.rate_limit_tokens_per_second = rate_limit_tokens_per_second;
  if (rate_limit_capacity > 0) {
    std::fprintf(stderr, "[gateway] rate limiting enabled: capacity=%.1f tokens_per_second=%.1f (per tenant, or per "
                        "device if a connection has no tenant_id)\n",
                 rate_limit_capacity, rate_limit_tokens_per_second);
  }
  IngestServiceImpl service(integrations);

  grpc::ServerBuilder builder;
  if (!tls_ca.empty() || !tls_cert.empty() || !tls_key.empty()) {
    if (tls_ca.empty() || tls_cert.empty() || tls_key.empty()) {
      std::fprintf(stderr, "[gateway] --tls-ca, --tls-cert, and --tls-key must all be provided together\n");
      return 2;
    }
    grpc::SslServerCredentialsOptions ssl_opts(GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
    ssl_opts.pem_root_certs = ReadFileOrDie(tls_ca);
    ssl_opts.pem_key_cert_pairs.push_back({ReadFileOrDie(tls_key), ReadFileOrDie(tls_cert)});
    builder.AddListeningPort(listen, grpc::SslServerCredentials(ssl_opts));
    std::fprintf(stderr, "[gateway] mTLS enabled: ca=%s cert=%s (client certs required and verified)\n", tls_ca.c_str(),
                 tls_cert.c_str());
  } else {
    builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  }
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) { std::fprintf(stderr, "[gateway] failed to listen on %s\n", listen.c_str()); return 1; }
  std::fprintf(stderr, "[gateway] listening on %s\n", listen.c_str());
  std::thread watcher([&] {
    while (!g_stop.load()) std::this_thread::sleep_for(100ms);
    server->Shutdown(std::chrono::system_clock::now() + 2s);
  });
  server->Wait(); watcher.join();
  std::fprintf(stderr, "[gateway] shut down\n");
  return 0;
}

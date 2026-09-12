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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include "ridgeline/time.h"
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

namespace {
using namespace std::chrono_literals;
using ridgeline::v1::AgentMessage;
using ridgeline::v1::GatewayMessage;
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }
bool g_log_events = false;  // Test oracle output; see scripts/chaos_test.sh.

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
};

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

  grpc::Status Connect(grpc::ServerContext*, grpc::ServerReaderWriter<GatewayMessage, AgentMessage>* stream) override {
    std::string device_id;
    std::uint64_t last_seq = 0, received = 0, duplicates = 0, gap_events = 0;
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
          device_id = msg.hello().device_id();
          last_seq = msg.hello().last_acked_seq();
          durable_resume = msg.hello().durable_resume();
          awaiting_resync = !durable_resume;
          std::fprintf(stderr, "[gateway] %s connected (resume after seq %llu)\n", device_id.c_str(),
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
          }
          GatewayMessage ack; ack.mutable_ack()->set_up_to_seq(last_seq);
          if (!stream->Write(ack)) return {grpc::StatusCode::UNAVAILABLE, "failed to write ack"};
          break;
        }
        case AgentMessage::kHeartbeat: {
          if (device_id.empty()) return {grpc::StatusCode::FAILED_PRECONDITION, "hello must be first"};
          const auto& hb = msg.heartbeat();
          (void)hb;  // Only read when Redis and/or DynamoDB integrations are compiled in (below); otherwise heartbeats
                     // are just validated (hello must precede them) and don't need to hold onto the payload.
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
          break;
        }
        case AgentMessage::PAYLOAD_NOT_SET:
          return {grpc::StatusCode::INVALID_ARGUMENT, "empty AgentMessage"};
      }
    }
    std::fprintf(stderr, "[gateway] %s disconnected: received=%llu duplicates=%llu lost=%llu max_transit=%.2fms",
                 device_id.empty() ? "<no hello>" : device_id.c_str(), static_cast<unsigned long long>(received),
                 static_cast<unsigned long long>(duplicates), static_cast<unsigned long long>(gap_events),
                 static_cast<double>(max_transit_ns) / 1e6);
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
};
}  // namespace

int main(int argc, char** argv) {
  std::string listen = "0.0.0.0:50051";
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

  IngestServiceImpl service(integrations);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
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

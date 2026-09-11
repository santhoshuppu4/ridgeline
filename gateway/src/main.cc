// Ridgeline ingest gateway — Phase 0 transport + Phase 1d-i Kafka publish.
//
// DURABILITY, MADE LITERAL: ADR-0001 said "an ack means durably stored" back
// when nothing backed that beyond the gateway process's own memory. With
// -DRIDGELINE_WITH_KAFKA=ON and --kafka-brokers set, the gateway now
// publishes each validated DetectionEvent to Kafka and only sends its Ack
// once KafkaProducer::PublishSync() confirms the broker accepted it. Without
// Kafka configured, behavior is unchanged from Phase 1c: ack on validation,
// exactly as before -- this is what keeps scripts/smoke_test.sh and
// scripts/chaos_test.sh passing with no Kafka involved at all.

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

namespace {
using namespace std::chrono_literals;
using ridgeline::v1::AgentMessage;
using ridgeline::v1::GatewayMessage;
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }
bool g_log_events = false;  // Test oracle output; see scripts/chaos_test.sh.

class IngestServiceImpl final : public ridgeline::v1::IngestService::Service {
 public:
#ifdef RIDGELINE_HAVE_KAFKA
  explicit IngestServiceImpl(ridgeline::KafkaProducer* kafka) : kafka_(kafka) {}
#endif

  grpc::Status Connect(grpc::ServerContext*, grpc::ServerReaderWriter<GatewayMessage, AgentMessage>* stream) override {
    std::string device_id;
    std::uint64_t last_seq = 0, received = 0, duplicates = 0, gap_events = 0;
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
            // A replayed event (agent resent something already acked): the
            // gateway already durably has it, so it is NOT republished to
            // Kafka. Republishing here would let a lossy connection turn
            // into duplicate downstream events -- the agent's WAL replay
            // and this gateway's dedup are what keep delivery
            // effectively-once from Kafka's point of view, riding on top of
            // gRPC's at-least-once.
          } else {
#ifdef RIDGELINE_HAVE_KAFKA
            if (kafka_ != nullptr) {
              // Publish BEFORE advancing last_seq / sending the ack: the ack
              // is the promise, and the promise must not be made until it's
              // true. If this returns false (broker down, timed out), the
              // gateway does NOT ack -- the agent's own WAL-backed resend
              // (ADR-0006) will retry this exact event on its next attempt,
              // exactly the same path used for a dropped connection.
              if (kafka_->PublishSync(device_id, d.SerializeAsString())) {
                ++kafka_published;
              } else {
                ++kafka_failed;
                break;  // No ack this round; do not advance last_seq. Agent will retry.
              }
            }
#endif
            if (d.seq() != last_seq + 1) gap_events += d.seq() - last_seq - 1;
            last_seq = d.seq(); ++received;
            max_transit_ns = std::max(max_transit_ns, ridgeline::NowUnixNs() - d.emit_time_unix_ns());
          }
          GatewayMessage ack; ack.mutable_ack()->set_up_to_seq(last_seq);
          if (!stream->Write(ack)) return {grpc::StatusCode::UNAVAILABLE, "failed to write ack"};
          break;
        }
        case AgentMessage::kHeartbeat:
          if (device_id.empty()) return {grpc::StatusCode::FAILED_PRECONDITION, "hello must be first"};
          break;
        case AgentMessage::PAYLOAD_NOT_SET:
          return {grpc::StatusCode::INVALID_ARGUMENT, "empty AgentMessage"};
      }
    }
    std::fprintf(stderr, "[gateway] %s disconnected: received=%llu duplicates=%llu lost=%llu max_transit=%.2fms",
                 device_id.empty() ? "<no hello>" : device_id.c_str(), static_cast<unsigned long long>(received),
                 static_cast<unsigned long long>(duplicates), static_cast<unsigned long long>(gap_events),
                 static_cast<double>(max_transit_ns) / 1e6);
#ifdef RIDGELINE_HAVE_KAFKA
    if (kafka_ != nullptr) {
      std::fprintf(stderr, " kafka_published=%llu kafka_failed=%llu", static_cast<unsigned long long>(kafka_published),
                   static_cast<unsigned long long>(kafka_failed));
    }
#endif
    std::fprintf(stderr, "\n");
    return grpc::Status::OK;
  }

 private:
#ifdef RIDGELINE_HAVE_KAFKA
  ridgeline::KafkaProducer* kafka_;
#endif
};
}  // namespace

int main(int argc, char** argv) {
  std::string listen = "0.0.0.0:50051";
#ifdef RIDGELINE_HAVE_KAFKA
  std::string kafka_brokers;
  std::string kafka_topic = "detections.v1";
  int kafka_timeout_ms = 5000;
#endif
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    auto value = [&](std::string_view key) -> const char* {
      return arg.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr;
    };
    if (arg.rfind("--listen=", 0) == 0) listen = std::string{arg.substr(9)};
    else if (arg == "--log-events") g_log_events = true;
#ifdef RIDGELINE_HAVE_KAFKA
    else if (auto v = value("--kafka-brokers=")) kafka_brokers = v;
    else if (auto v2 = value("--kafka-topic=")) kafka_topic = v2;
    else if (auto v3 = value("--kafka-timeout-ms=")) kafka_timeout_ms = std::atoi(v3);
#endif
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
  }
  std::signal(SIGINT, OnSignal); std::signal(SIGTERM, OnSignal);

#ifdef RIDGELINE_HAVE_KAFKA
  std::unique_ptr<ridgeline::KafkaProducer> kafka;
  if (!kafka_brokers.empty()) {
    ridgeline::KafkaProducerConfig kcfg;
    kcfg.brokers = kafka_brokers;
    kcfg.topic = kafka_topic;
    kcfg.delivery_timeout_ms = kafka_timeout_ms;
    try {
      kafka = std::make_unique<ridgeline::KafkaProducer>(kcfg);
      std::fprintf(stderr, "[gateway] Kafka publish enabled: brokers=%s topic=%s\n", kafka_brokers.c_str(), kafka_topic.c_str());
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[gateway] failed to create Kafka producer: %s\n", e.what());
      return 1;
    }
  }
  IngestServiceImpl service(kafka.get());
#else
  IngestServiceImpl service;
#endif

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

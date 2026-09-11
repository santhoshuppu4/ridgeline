#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include "ridgeline/time.h"
#include "ridgeline/v1/ingest.grpc.pb.h"

namespace {
using namespace std::chrono_literals;
using ridgeline::v1::AgentMessage;
using ridgeline::v1::GatewayMessage;
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

class IngestServiceImpl final : public ridgeline::v1::IngestService::Service {
 public:
  grpc::Status Connect(grpc::ServerContext*, grpc::ServerReaderWriter<GatewayMessage, AgentMessage>* stream) override {
    std::string device_id;
    std::uint64_t last_seq = 0, received = 0, duplicates = 0, gap_events = 0;
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
          if (d.seq() <= last_seq) { ++duplicates; }
          else {
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
    std::fprintf(stderr, "[gateway] %s disconnected: received=%llu duplicates=%llu lost=%llu max_transit=%.2fms\n",
                 device_id.empty() ? "<no hello>" : device_id.c_str(), static_cast<unsigned long long>(received),
                 static_cast<unsigned long long>(duplicates), static_cast<unsigned long long>(gap_events),
                 static_cast<double>(max_transit_ns) / 1e6);
    return grpc::Status::OK;
  }
};
}  // namespace

int main(int argc, char** argv) {
  std::string listen = "0.0.0.0:50051";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg.rfind("--listen=", 0) == 0) listen = std::string{arg.substr(9)};
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
  }
  std::signal(SIGINT, OnSignal); std::signal(SIGTERM, OnSignal);
  IngestServiceImpl service;
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

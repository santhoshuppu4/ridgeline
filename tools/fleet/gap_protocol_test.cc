// ridgeline_gap_protocol_test: a minimal, direct gRPC client used ONLY to
// prove ADR-0010's fix is correct in BOTH directions -- not a general tool.
//
// Sends exactly one Hello and a hand-picked sequence of DetectionEvents,
// then disconnects. Lets a test script construct precise seq-number gaps
// and durable_resume values that would be awkward to force through the
// real agent (WAL-driven) or the simulator (always durable_resume=false)
// and then check the gateway's own log output for the expected "lost="
// value -- the same log-based oracle style used by chaos_test.sh.
//
// Usage:
//   ./ridgeline_gap_protocol_test --gateway=127.0.0.1:PORT --device-id=X --durable-resume=true|false --last-acked-seq=N --send-seq=M
//
// Sends one Hello (with the given last_acked_seq/durable_resume), then one
// DetectionEvent with seq=send-seq, then closes the stream.

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "ridgeline/time.h"
#include "ridgeline/v1/ingest.grpc.pb.h"

int main(int argc, char** argv) {
  std::string gateway = "127.0.0.1:50051";
  std::string device_id = "gap-test";
  bool durable_resume = true;
  std::uint64_t last_acked_seq = 0;
  std::uint64_t send_seq = 1;

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    auto val = [&](std::string_view key) { return a.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr; };
    if (auto v = val("--gateway=")) gateway = v;
    else if (auto v2 = val("--device-id=")) device_id = v2;
    else if (auto v3 = val("--durable-resume=")) durable_resume = (std::string_view(v3) == "true");
    else if (auto v4 = val("--last-acked-seq=")) last_acked_seq = std::strtoull(v4, nullptr, 10);
    else if (auto v5 = val("--send-seq=")) send_seq = std::strtoull(v5, nullptr, 10);
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
  }

  auto channel = grpc::CreateChannel(gateway, grpc::InsecureChannelCredentials());
  if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(3))) {
    std::fprintf(stderr, "could not connect to %s\n", gateway.c_str());
    return 1;
  }
  auto stub = ridgeline::v1::IngestService::NewStub(channel);
  grpc::ClientContext ctx;
  auto stream = stub->Connect(&ctx);

  ridgeline::v1::AgentMessage hello;
  hello.mutable_hello()->set_device_id(device_id);
  hello.mutable_hello()->set_agent_version("gap-protocol-test");
  hello.mutable_hello()->set_last_acked_seq(last_acked_seq);
  hello.mutable_hello()->set_durable_resume(durable_resume);
  if (!stream->Write(hello)) { std::fprintf(stderr, "failed to write hello\n"); return 1; }

  ridgeline::v1::AgentMessage detection;
  auto* d = detection.mutable_detection();
  d->set_seq(send_seq);
  d->set_label("smoke");
  d->set_confidence(0.9f);
  const auto ts = ridgeline::NowUnixNs();
  d->set_capture_time_unix_ns(ts);
  d->set_emit_time_unix_ns(ts);
  if (!stream->Write(detection)) { std::fprintf(stderr, "failed to write detection\n"); return 1; }

  ridgeline::v1::GatewayMessage ack;
  if (!stream->Read(&ack)) { std::fprintf(stderr, "no ack received\n"); return 1; }

  stream->WritesDone();
  stream->Finish();
  return 0;
}

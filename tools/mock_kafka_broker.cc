// ridgeline_mock_kafka_broker: runs librdkafka's real, wire-protocol-compatible
// mock cluster as a standalone process, and prints its bootstrap address.
//
// WHY THIS EXISTS: the project's deploy/docker-compose.yml brings up a real
// Redpanda broker for production-realistic testing, but that needs Docker,
// which isn't available everywhere (this project's own CI sandbox included).
// This tool needs nothing but librdkafka itself -- already a build
// dependency -- and speaks the actual Kafka wire protocol, not a fake. It's
// what scripts/kafka_gateway_smoke_test.sh uses to run ridgeline_gateway's
// real Kafka integration end-to-end with no external services.
//
// It is NOT a substitute for testing against a real multi-broker cluster
// before trusting production behavior (partition rebalancing, real network
// partitions, disk-backed durability) -- see context/adr/0007 for exactly
// what this does and doesn't prove.
//
// Usage:
//   ./ridgeline_mock_kafka_broker
//   # prints, e.g.: BOOTSTRAP=127.0.0.1:41234
//   # then blocks until SIGINT/SIGTERM.

#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }
}  // namespace

int main() {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  char errstr[512];
  rd_kafka_conf_t* conf = rd_kafka_conf_new();
  rd_kafka_t* owner = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
  if (!owner) {
    std::fprintf(stderr, "rd_kafka_new failed: %s\n", errstr);
    return 1;
  }
  rd_kafka_mock_cluster_t* cluster = rd_kafka_mock_cluster_new(owner, /*broker_cnt=*/1);
  if (!cluster) {
    std::fprintf(stderr, "rd_kafka_mock_cluster_new failed\n");
    rd_kafka_destroy(owner);
    return 1;
  }

  // Machine-parseable on its own line, so a shell script can grab it with
  // e.g. `grep -m1 BOOTSTRAP= | cut -d= -f2`.
  std::printf("BOOTSTRAP=%s\n", rd_kafka_mock_cluster_bootstraps(cluster));
  std::fflush(stdout);
  std::fprintf(stderr, "[mock-kafka] running (Ctrl+C to stop)\n");

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::fprintf(stderr, "[mock-kafka] shutting down\n");
  rd_kafka_mock_cluster_destroy(cluster);
  rd_kafka_destroy(owner);
  return 0;
}

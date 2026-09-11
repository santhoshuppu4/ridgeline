// Tests run against librdkafka's built-in mock cluster (rdkafka_mock.h),
// which implements the real Kafka wire protocol in-process -- not a
// hand-rolled fake. See context/adr/0007-kafka-event-backbone.md for exactly
// what that does and doesn't prove relative to a real Redpanda/Kafka
// cluster, and how to run the same PublishSync() code against one.

#include "ridgeline/kafka_producer.h"

#include <gtest/gtest.h>
#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>
#include <librdkafka/rdkafkacpp.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace {

// Spins up an in-process mock broker and tears it down on destruction. A
// fresh instance per test -- broker state (topics, offsets) isn't reset
// between tests otherwise, which would make tests order-dependent.
class MockKafkaCluster {
 public:
  MockKafkaCluster() {
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    // A bare handle purely to own the mock cluster; never produces/consumes
    // through it directly -- ridgeline::KafkaProducer gets its OWN handle,
    // pointed at this cluster's bootstrap address, exactly like a real
    // client connecting to a real cluster over the network.
    owner_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!owner_) throw std::runtime_error(std::string("rd_kafka_new: ") + errstr);
    cluster_ = rd_kafka_mock_cluster_new(owner_, /*broker_cnt=*/1);
    if (!cluster_) throw std::runtime_error("rd_kafka_mock_cluster_new failed");
  }
  ~MockKafkaCluster() {
    if (cluster_) rd_kafka_mock_cluster_destroy(cluster_);
    if (owner_) rd_kafka_destroy(owner_);
  }
  MockKafkaCluster(const MockKafkaCluster&) = delete;
  MockKafkaCluster& operator=(const MockKafkaCluster&) = delete;

  std::string Bootstrap() const { return rd_kafka_mock_cluster_bootstraps(cluster_); }
  rd_kafka_mock_cluster_t* raw() const { return cluster_; }

 private:
  rd_kafka_t* owner_ = nullptr;
  rd_kafka_mock_cluster_t* cluster_ = nullptr;
};

ridgeline::KafkaProducerConfig ConfigFor(const MockKafkaCluster& cluster, const std::string& topic) {
  ridgeline::KafkaProducerConfig cfg;
  cfg.brokers = cluster.Bootstrap();
  cfg.topic = topic;
  cfg.delivery_timeout_ms = 3000;
  cfg.acks = "all";
  return cfg;
}

}  // namespace

TEST(KafkaProducer, PublishSyncSucceedsAgainstRealMockBroker) {
  MockKafkaCluster cluster;
  ridgeline::KafkaProducer producer(ConfigFor(cluster, "test-topic-1"));

  EXPECT_TRUE(producer.PublishSync("device-1", "payload-a"));
  EXPECT_TRUE(producer.PublishSync("device-1", "payload-b"));
  EXPECT_TRUE(producer.PublishSync("device-2", "payload-c"));

  EXPECT_EQ(producer.Stats().published.load(), 3u);
  EXPECT_EQ(producer.Stats().failed.load(), 0u);
  EXPECT_EQ(producer.Stats().timed_out.load(), 0u);
}

TEST(KafkaProducer, ManyMessagesAllConfirmedInOrder) {
  // Not a throughput benchmark -- that's a job for bench/, once a real
  // cluster is available (see ADR-0007). This just proves PublishSync's
  // per-call delivery tracking (the PendingDelivery mechanism) is correct
  // under many concurrent-in-flight-queue messages, not just a lone one.
  MockKafkaCluster cluster;
  ridgeline::KafkaProducer producer(ConfigFor(cluster, "test-topic-2"));

  constexpr int kCount = 500;
  int confirmed = 0;
  for (int i = 0; i < kCount; ++i) {
    if (producer.PublishSync("device-x", "event-" + std::to_string(i))) ++confirmed;
  }
  EXPECT_EQ(confirmed, kCount);
  EXPECT_EQ(producer.Stats().published.load(), static_cast<std::uint64_t>(kCount));
}

TEST(KafkaProducer, ConcurrentPublishersFromMultipleThreadsAllSucceed) {
  // The gateway's synchronous produce-then-ack design (see ADR-0007) means
  // this exact pattern -- multiple device connections each on their own
  // thread, each calling PublishSync -- is the real production shape.
  MockKafkaCluster cluster;
  ridgeline::KafkaProducer producer(ConfigFor(cluster, "test-topic-3"));

  constexpr int kThreads = 8;
  constexpr int kPerThread = 50;
  std::atomic<int> total_confirmed{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        if (producer.PublishSync("device-" + std::to_string(t), "msg-" + std::to_string(i))) {
          total_confirmed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(total_confirmed.load(), kThreads * kPerThread);
  EXPECT_EQ(producer.Stats().published.load(), static_cast<std::uint64_t>(kThreads * kPerThread));
}

TEST(KafkaProducer, UnreachableBrokerFailsRatherThanHangingForever) {
  // No mock cluster here on purpose: points at a real TCP port with nothing
  // listening. A short delivery_timeout_ms must make PublishSync return
  // false in bounded time, not hang -- this is what protects the gateway
  // from stalling forever if Kafka is genuinely down.
  ridgeline::KafkaProducerConfig cfg;
  cfg.brokers = "127.0.0.1:1";  // Port 1 is reserved/unlikely to have anything bound.
  cfg.topic = "unreachable-topic";
  cfg.delivery_timeout_ms = 800;

  ridgeline::KafkaProducer producer(cfg);
  const auto start = std::chrono::steady_clock::now();
  const bool ok = producer.PublishSync("device-1", "payload");
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_FALSE(ok);
  EXPECT_LT(elapsed, std::chrono::milliseconds(3000)) << "must fail within roughly delivery_timeout_ms, not hang";
}

TEST(KafkaProducer, ConstructorThrowsOnMalformedConfig) {
  ridgeline::KafkaProducerConfig cfg;
  cfg.acks = "not-a-valid-acks-value";  // librdkafka validates this at Conf::set() time.
  EXPECT_THROW(ridgeline::KafkaProducer{cfg}, std::runtime_error);
}

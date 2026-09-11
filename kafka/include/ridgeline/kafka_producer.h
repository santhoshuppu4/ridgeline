#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace RdKafka {  // Forward declarations so this header doesn't force every
class Producer;      // includer to pull in <librdkafka/rdkafkacpp.h>.
class Conf;
class DeliveryReportCb;
}  // namespace RdKafka

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHAT THIS IS FOR: the gateway currently acks a DetectionEvent the moment it
// validates the sequence number (ADR-0001: "an ack means durably stored").
// That was aspirational until now -- nothing was actually durable past the
// gateway process's own memory. KafkaProducer is what makes "durably stored"
// literal: PublishSync() blocks until Kafka's own delivery-report callback
// confirms the broker accepted the write (or times out), and the gateway only
// sends its Ack to the agent after that returns true. If the gateway crashes
// between accepting a detection and Kafka acking it, the AGENT still has the
// event in its own WAL (ADR-0006) and will resend on reconnect -- durability
// now spans two independent layers instead of one.
//
// WHY SYNCHRONOUS, NOT FIRE-AND-FORGET: librdkafka's Producer::produce() is
// asynchronous by default -- it queues the message and returns immediately,
// with real delivery confirmed later via a callback. For our use (the ack to
// the agent depends on the outcome), that async gap needs to be closed
// somewhere. PublishSync() closes it by producing, then polling until this
// specific message's delivery report arrives or a timeout elapses. This
// trades throughput for a simple, correct integration point; a
// higher-throughput design would decouple ack-to-agent from Kafka delivery
// and use the WAL as the sole durability source until Kafka confirms
// out-of-band -- a real future optimization, deliberately not done here so
// the correctness story stays easy to state and to test.
//
// PARTITIONING: keyed by device_id. The original design doc calls for
// `tenant_id:device_id` once multi-tenancy exists (Phase 3); until then,
// device_id alone preserves per-device ordering, which is the property that
// actually matters for a downstream consumer replaying one camera's events
// in order.
//
// TESTED AGAINST A REAL BROKER PROTOCOL, NOT A FAKE: librdkafka ships an
// in-process mock cluster (rdkafka_mock.h) that speaks the actual Kafka wire
// protocol -- tests/kafka_producer_test.cc runs against that, not a
// hand-rolled stub. See context/adr/0007 for what that does and doesn't
// prove versus a real multi-broker Redpanda/Kafka cluster.
// ---------------------------------------------------------------------------

struct KafkaProducerConfig {
  std::string brokers = "localhost:19092";  // Matches deploy/docker-compose.yml's Redpanda external listener.
  std::string topic = "detections.v1";
  int delivery_timeout_ms = 5000;
  std::string acks = "all";  // "all" = broker waits for every in-sync replica; matches the durability claim above.
};
struct KafkaPublishStats {
  std::atomic<std::uint64_t> published{0};    // Delivery report confirmed success.
  std::atomic<std::uint64_t> failed{0};       // Delivery report confirmed failure (broker rejected/errored).
  std::atomic<std::uint64_t> timed_out{0};    // No delivery report within delivery_timeout_ms (unknown outcome).
};

class KafkaProducer {
 public:
  // Throws std::runtime_error if the underlying producer can't be constructed
  // (e.g. malformed config) -- constructing a client that will silently never
  // work is worse than failing at startup.
  explicit KafkaProducer(KafkaProducerConfig config);
  ~KafkaProducer();

  KafkaProducer(const KafkaProducer&) = delete;
  KafkaProducer& operator=(const KafkaProducer&) = delete;

  // Publishes `value` under partition key `key`, blocking until this specific
  // message's delivery report arrives or delivery_timeout_ms elapses.
  // Returns true only on confirmed broker acceptance.
  bool PublishSync(const std::string& key, const std::string& value);

  const KafkaPublishStats& Stats() const { return stats_; }

  // Flushes any producer-internal queue and waits up to timeout_ms. Call
  // before destruction if PublishSync isn't guaranteed to have drained
  // everything (it normally has, since it's synchronous per-call).
  void Flush(int timeout_ms);

 private:
  class DeliveryCb;  // PIMPL: keeps <rdkafkacpp.h> out of this header.

  KafkaProducerConfig config_;
  KafkaPublishStats stats_;
  std::unique_ptr<RdKafka::Producer> producer_;
  std::unique_ptr<DeliveryCb> delivery_cb_;
};

}  // namespace ridgeline

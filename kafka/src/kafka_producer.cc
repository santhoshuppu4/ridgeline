#include "ridgeline/kafka_producer.h"

#include <librdkafka/rdkafkacpp.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace ridgeline {

namespace {

// STUDY NOTE -- a real bug this design fixes, worth understanding:
//
// The first version of this file put PendingDelivery on PublishSync's own
// stack, passed a raw pointer to it via msg_opaque, and deleted nothing --
// relying on dr_cb() firing before PublishSync's own wait loop gave up.
// That's true on the happy path. It's NOT true when the broker is
// unreachable: PublishSync's local timeout returns false well before
// librdkafka internally gives up on the message (which, depending on
// librdkafka's connection-retry behavior, can take far longer than
// message.timeout.ms suggests when the client can't even reach a broker to
// get topic metadata). The stack frame is gone by the time dr_cb() actually
// fires and writes into it: a use-after-free, not merely a slow test.
// Caught by tests/kafka/kafka_producer_test.cc's
// UnreachableBrokerFailsRatherThanHangingForever running for 90+ seconds
// instead of the expected ~1s -- a hang, not a crash, because in this
// build the freed stack memory happened to still be readable/writable, but
// under different timing or an ASan build this is exactly the kind of bug
// that segfaults or gets flagged as heap/stack-use-after-{free,return}.
//
// FIX: PendingDelivery is heap-allocated and reference-counted via
// shared_ptr. PublishSync keeps one reference for as long as IT cares about
// the outcome; a second reference travels through librdkafka via
// msg_opaque, wrapped in its own heap allocation so it can be reconstructed
// from a raw void*. Each side drops its own reference when it's done with
// the object; the underlying PendingDelivery is only freed once BOTH sides
// are done, whichever finishes last -- so a local timeout can safely
// stop waiting without invalidating what a much-later dr_cb() call will
// write into.
struct PendingDelivery {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  bool success = false;
};

using PendingDeliveryPtr = std::shared_ptr<PendingDelivery>;

}  // namespace

class KafkaProducer::DeliveryCb : public RdKafka::DeliveryReportCb {
 public:
  explicit DeliveryCb(KafkaPublishStats& stats) : stats_(stats) {}

  void dr_cb(RdKafka::Message& message) override {
    // Reclaim the heap-allocated shared_ptr wrapper handed to produce() as
    // msg_opaque, and make sure it's freed (dropping this side's reference)
    // no matter which branch below runs.
    std::unique_ptr<PendingDeliveryPtr> wrapper(static_cast<PendingDeliveryPtr*>(message.msg_opaque()));
    const bool ok = message.err() == RdKafka::ERR_NO_ERROR;
    if (ok) stats_.published.fetch_add(1, std::memory_order_relaxed);
    else stats_.failed.fetch_add(1, std::memory_order_relaxed);

    if (wrapper) {
      std::lock_guard<std::mutex> lock((*wrapper)->mu);
      (*wrapper)->success = ok;
      (*wrapper)->done = true;
      (*wrapper)->cv.notify_one();
    }
    // wrapper's destructor runs here, dropping this side's shared_ptr
    // reference. If PublishSync already gave up and dropped its own
    // reference, this is the call that actually frees the PendingDelivery.
  }

 private:
  KafkaPublishStats& stats_;
};

KafkaProducer::KafkaProducer(KafkaProducerConfig config) : config_(std::move(config)) {
  delivery_cb_ = std::make_unique<DeliveryCb>(stats_);

  std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  std::string errstr;

  auto set = [&](const std::string& key, const std::string& value) {
    if (conf->set(key, value, errstr) != RdKafka::Conf::CONF_OK) {
      throw std::runtime_error("kafka config '" + key + "'='" + value + "': " + errstr);
    }
  };
  set("bootstrap.servers", config_.brokers);
  set("acks", config_.acks);
  // Fail produce() calls quickly when brokers are genuinely unreachable
  // rather than retrying silently for librdkafka's default (very long)
  // window -- PublishSync()'s own timeout is what callers actually observe,
  // but a short message timeout keeps librdkafka's internal retry loop from
  // masking a real outage as "still pending."
  set("message.timeout.ms", std::to_string(config_.delivery_timeout_ms));

  if (conf->set("dr_cb", delivery_cb_.get(), errstr) != RdKafka::Conf::CONF_OK) {
    throw std::runtime_error("kafka config 'dr_cb': " + errstr);
  }

  producer_.reset(RdKafka::Producer::create(conf.get(), errstr));
  if (!producer_) {
    throw std::runtime_error("failed to create Kafka producer: " + errstr);
  }
}

KafkaProducer::~KafkaProducer() {
  if (producer_) Flush(std::max(1000, config_.delivery_timeout_ms));
}

bool KafkaProducer::PublishSync(const std::string& key, const std::string& value) {
  auto pending = std::make_shared<PendingDelivery>();
  // Heap-allocate a SECOND shared_ptr, owned by librdkafka's msg_opaque until
  // dr_cb() reclaims and deletes it. This is what keeps `pending` alive if
  // PublishSync gives up before dr_cb ever fires -- see the file-level note
  // on the use-after-free this replaced.
  auto* opaque = new PendingDeliveryPtr(pending);

  const RdKafka::ErrorCode err = producer_->produce(
      config_.topic, RdKafka::Topic::PARTITION_UA /* let the partitioner key on `key` */,
      RdKafka::Producer::RK_MSG_COPY /* librdkafka copies value/key; safe to let them go out of scope */,
      const_cast<char*>(value.data()), value.size(), key.data(), key.size(),
      0 /* timestamp: broker/library default (now) */, opaque);

  if (err != RdKafka::ERR_NO_ERROR) {
    // Rejected synchronously (e.g. queue full): librdkafka will NOT call
    // dr_cb for a message it never accepted, so no callback will ever
    // reclaim `opaque` -- we must free it here ourselves, or it leaks.
    delete opaque;
    stats_.failed.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.delivery_timeout_ms);
  std::unique_lock<std::mutex> lock(pending->mu);
  // poll() must run on THIS thread (or another) for dr_cb to ever fire --
  // librdkafka delivery reports are dispatched from inside poll(), not from
  // a background thread, unless the application sets up its own event
  // thread. Polling in a loop here keeps this class simple/synchronous.
  while (!pending->done) {
    lock.unlock();
    producer_->poll(50);
    lock.lock();
    if (!pending->done && std::chrono::steady_clock::now() >= deadline) {
      // Give up WITHOUT touching `opaque` -- it's still reachable through
      // librdkafka's internal queue, and `pending`'s shared_ptr refcount
      // keeps the object alive until dr_cb (eventually) runs and drops its
      // own reference. `pending` (this function's local shared_ptr) goes
      // out of scope on return either way; that alone does not free the
      // object as long as the opaque-held reference is still outstanding.
      stats_.timed_out.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  return pending->success;
}

void KafkaProducer::Flush(int timeout_ms) { producer_->flush(timeout_ms); }

}  // namespace ridgeline

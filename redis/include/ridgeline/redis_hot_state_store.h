#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHAT THIS IS FOR: "hot" per-device state that changes constantly (last-seen
// timestamp, current frame-ring queue depth, cumulative dropped-frame count)
// and that a live dashboard or health check wants to read cheaply, without
// hitting the gateway process directly or waiting on a DynamoDB round trip.
// This is deliberately NOT the durable device shadow (that's
// DeviceShadowStore, backed by DynamoDB) -- Redis here is a fast, ephemeral
// cache: if it's lost (process restart, cache eviction), nothing is lost
// that matters, because it's rebuilt from the next heartbeat.
//
// STORAGE SHAPE: one Redis HASH per device, key `device:<device_id>`, with
// fields last_seen_unix_ns, queue_depth, frames_dropped. A hash (not
// separate string keys per field) means one round trip reads or writes the
// whole device state, and HGETALL naturally returns a struct-shaped result.
//
// WHY hiredis DIRECTLY, NOT A HIGHER-LEVEL C++ WRAPPER LIBRARY: hiredis is
// the reference C client every other C++ Redis library wraps anyway, it's
// small, well-understood, and avoids adding a dependency with its own
// opinions about threading/connection pooling that this class doesn't need.
//
// THREAD SAFETY: a single hiredis connection is NOT thread-safe for
// concurrent commands. RedisHotStateStore serializes access with an internal
// mutex -- simple and correct for this project's call volume (per-heartbeat,
// not per-frame). A high-throughput redesign would use a connection pool;
// not needed here, and saying so explicitly beats silently limiting scale
// without explanation.
// ---------------------------------------------------------------------------

struct DeviceHotState {
  std::int64_t last_seen_unix_ns = 0;
  std::uint32_t queue_depth = 0;
  std::uint64_t frames_dropped = 0;
};

struct RedisConfig {
  std::string host = "localhost";
  int port = 6379;
  int connect_timeout_ms = 2000;
  int command_timeout_ms = 1000;
};

class RedisHotStateStore {
 public:
  // Throws std::runtime_error if the initial connection fails -- consistent
  // with KafkaProducer/DeviceShadowStore: fail loudly at construction rather
  // than silently limping along with a client that will never work.
  explicit RedisHotStateStore(RedisConfig config);
  ~RedisHotStateStore();

  RedisHotStateStore(const RedisHotStateStore&) = delete;
  RedisHotStateStore& operator=(const RedisHotStateStore&) = delete;

  // Upserts the full hash in one round trip (HSET with multiple fields).
  // Returns false on any Redis-level error (connection drop, command
  // error) -- callers should treat this cache as best-effort, per the
  // "ephemeral, not durable" design above: a failed write here should never
  // block or fail whatever real work (acking a detection) triggered it.
  bool Update(const std::string& device_id, const DeviceHotState& state);

  // Returns nullopt if the device has no hash yet (never seen) OR on any
  // Redis-level error -- deliberately not distinguished, since both mean
  // "no reliable state to show," and a cache miss is not itself an error
  // worth surfacing differently to a caller that's just trying to render a
  // dashboard.
  std::optional<DeviceHotState> Get(const std::string& device_id);

  // Reconnects using the original config. Call after a sustained run of
  // failures; hiredis does not auto-reconnect. Returns false if the new
  // connection attempt itself fails (this store remains unusable until a
  // later successful Reconnect()).
  bool Reconnect();

 private:
  class Connection;  // PIMPL: keeps <hiredis/hiredis.h> out of this header.
  RedisConfig config_;
  std::unique_ptr<Connection> conn_;
};

}  // namespace ridgeline

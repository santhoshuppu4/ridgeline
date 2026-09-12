#include "ridgeline/redis_hot_state_store.h"

#include <hiredis/hiredis.h>

#include <mutex>
#include <stdexcept>

namespace ridgeline {

namespace {

// RAII around a redisReply* -- hiredis requires every reply to be freed with
// freeReplyObject(), and every early-return path below (error checks) makes
// a raw pointer easy to leak without this.
struct ReplyGuard {
  redisReply* reply;
  explicit ReplyGuard(redisReply* r) : reply(r) {}
  ~ReplyGuard() { if (reply) freeReplyObject(reply); }
  ReplyGuard(const ReplyGuard&) = delete;
  ReplyGuard& operator=(const ReplyGuard&) = delete;
};

}  // namespace

class RedisHotStateStore::Connection {
 public:
  explicit Connection(const RedisConfig& config) {
    const struct timeval connect_tv {
      config.connect_timeout_ms / 1000, (config.connect_timeout_ms % 1000) * 1000
    };
    ctx_ = redisConnectWithTimeout(config.host.c_str(), config.port, connect_tv);
    if (ctx_ == nullptr) {
      throw std::runtime_error("redisConnectWithTimeout: allocation failed");
    }
    if (ctx_->err) {
      std::string err = ctx_->errstr;
      redisFree(ctx_);
      ctx_ = nullptr;
      throw std::runtime_error("redis connection failed: " + err);
    }
    const struct timeval cmd_tv {
      config.command_timeout_ms / 1000, (config.command_timeout_ms % 1000) * 1000
    };
    redisSetTimeout(ctx_, cmd_tv);
  }
  ~Connection() {
    if (ctx_) redisFree(ctx_);
  }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  redisContext* raw() const { return ctx_; }

 private:
  redisContext* ctx_ = nullptr;
};

RedisHotStateStore::RedisHotStateStore(RedisConfig config)
    : config_(std::move(config)), conn_(std::make_unique<Connection>(config_)) {}

RedisHotStateStore::~RedisHotStateStore() = default;

bool RedisHotStateStore::Update(const std::string& device_id, const DeviceHotState& state) {
  static std::mutex mu;  // See header: one hiredis connection, serialized access.
  std::lock_guard<std::mutex> lock(mu);

  const std::string key = "device:" + device_id;
  // HSET key f1 v1 f2 v2 f3 v3 -- one round trip for the whole struct.
  redisReply* raw = static_cast<redisReply*>(redisCommand(
      conn_->raw(), "HSET %s last_seen_unix_ns %lld queue_depth %u frames_dropped %llu", key.c_str(),
      static_cast<long long>(state.last_seen_unix_ns), state.queue_depth,
      static_cast<unsigned long long>(state.frames_dropped)));
  ReplyGuard guard(raw);
  if (raw == nullptr) return false;  // I/O error (e.g. connection dropped); see Reconnect().
  if (raw->type == REDIS_REPLY_ERROR) return false;
  return true;
}

std::optional<DeviceHotState> RedisHotStateStore::Get(const std::string& device_id) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);

  const std::string key = "device:" + device_id;
  redisReply* raw = static_cast<redisReply*>(redisCommand(conn_->raw(), "HGETALL %s", key.c_str()));
  ReplyGuard guard(raw);
  if (raw == nullptr || raw->type == REDIS_REPLY_ERROR) return std::nullopt;
  if (raw->type != REDIS_REPLY_ARRAY || raw->elements == 0) return std::nullopt;  // Key doesn't exist.

  DeviceHotState state;
  // HGETALL returns a flat [field1, value1, field2, value2, ...] array.
  for (std::size_t i = 0; i + 1 < raw->elements; i += 2) {
    const std::string field = raw->element[i]->str ? raw->element[i]->str : "";
    const std::string value = raw->element[i + 1]->str ? raw->element[i + 1]->str : "";
    try {
      if (field == "last_seen_unix_ns") state.last_seen_unix_ns = std::stoll(value);
      else if (field == "queue_depth") state.queue_depth = static_cast<std::uint32_t>(std::stoul(value));
      else if (field == "frames_dropped") state.frames_dropped = std::stoull(value);
    } catch (const std::exception&) {
      // A field that fails to parse (corrupt/foreign data in this key) is
      // treated as "value missing," not as a reason to fail the whole read
      // -- the other fields parsed so far remain in `state`. Consistent
      // with this store's "best-effort cache" contract (see header).
    }
  }
  return state;
}

bool RedisHotStateStore::Reconnect() {
  try {
    conn_ = std::make_unique<Connection>(config_);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace ridgeline

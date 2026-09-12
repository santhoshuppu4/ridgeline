// Runs a REAL redis-server binary as a subprocess (not a mock, not a fake --
// the actual production Redis server, same as scripts would install on a
// real box) for the duration of this test binary. See
// context/adr/0008-hot-state-and-device-shadow.md for why: unlike Kafka's
// mock cluster (a librdkafka-internal test tool) or DynamoDB's fake
// transport (necessary because this project's sandbox has zero AWS/Docker
// network access), Redis itself is a lightweight, dependency-free apt
// package -- there's no reason to test against anything less than the real
// thing.

#include "ridgeline/redis_hot_state_store.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

namespace {

class RealRedisServer {
 public:
  RealRedisServer() {
    port_ = 20000 + (static_cast<int>(getpid()) % 10000);  // Spread across parallel test runs/retries.
    pid_ = fork();
    if (pid_ == 0) {
      // Child: exec the real redis-server binary, daemonized off, logging
      // to /dev/null, bound only to loopback on a test-specific port so
      // parallel CI runs (or a real redis-server already running on 6379)
      // never collide with this test.
      const std::string port_str = std::to_string(port_);
      execlp("redis-server", "redis-server", "--port", port_str.c_str(), "--bind", "127.0.0.1", "--daemonize", "no",
             "--save", "", "--appendonly", "no", "--logfile", "/dev/null", static_cast<char*>(nullptr));
      std::_Exit(127);  // exec failed (redis-server not installed) -- parent detects via connection failure below.
    }
    // Parent: give the server a moment to bind and start accepting
    // connections. Polling with actual connection attempts (rather than a
    // fixed sleep) would be more robust; a short fixed wait keeps this test
    // fixture simple, and redis-server's startup is consistently fast.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  ~RealRedisServer() {
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      int status = 0;
      waitpid(pid_, &status, 0);
    }
  }

  RealRedisServer(const RealRedisServer&) = delete;
  RealRedisServer& operator=(const RealRedisServer&) = delete;

  int port() const { return port_; }

 private:
  pid_t pid_ = -1;
  int port_ = 0;
};

ridgeline::RedisConfig ConfigFor(const RealRedisServer& server) {
  ridgeline::RedisConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = server.port();
  return cfg;
}

}  // namespace

TEST(RedisHotStateStore, GetOnNeverSeenDeviceReturnsNullopt) {
  RealRedisServer server;
  ridgeline::RedisHotStateStore store(ConfigFor(server));
  EXPECT_FALSE(store.Get("never-seen-device").has_value());
}

TEST(RedisHotStateStore, UpdateThenGetRoundTrips) {
  RealRedisServer server;
  ridgeline::RedisHotStateStore store(ConfigFor(server));

  ridgeline::DeviceHotState state;
  state.last_seen_unix_ns = 1789160000000000000LL;
  state.queue_depth = 3;
  state.frames_dropped = 42;

  ASSERT_TRUE(store.Update("cam-1", state));
  const auto got = store.Get("cam-1");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->last_seen_unix_ns, state.last_seen_unix_ns);
  EXPECT_EQ(got->queue_depth, state.queue_depth);
  EXPECT_EQ(got->frames_dropped, state.frames_dropped);
}

TEST(RedisHotStateStore, UpdateOverwritesPreviousStateForSameDevice) {
  RealRedisServer server;
  ridgeline::RedisHotStateStore store(ConfigFor(server));

  store.Update("cam-1", {100, 1, 0});
  store.Update("cam-1", {200, 5, 3});

  const auto got = store.Get("cam-1");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->last_seen_unix_ns, 200);
  EXPECT_EQ(got->queue_depth, 5u);
  EXPECT_EQ(got->frames_dropped, 3u);
}

TEST(RedisHotStateStore, DifferentDevicesAreIndependent) {
  RealRedisServer server;
  ridgeline::RedisHotStateStore store(ConfigFor(server));

  store.Update("cam-1", {111, 1, 1});
  store.Update("cam-2", {222, 2, 2});

  EXPECT_EQ(store.Get("cam-1")->queue_depth, 1u);
  EXPECT_EQ(store.Get("cam-2")->queue_depth, 2u);
}

TEST(RedisHotStateStore, ManySequentialUpdatesAllVisible) {
  // Not a throughput benchmark; just proves the request/response framing
  // holds up over more than a handful of round trips (an off-by-one in
  // reply parsing tends to show up around buffer boundaries, not on call 1).
  RealRedisServer server;
  ridgeline::RedisHotStateStore store(ConfigFor(server));

  for (std::uint32_t i = 0; i < 500; ++i) {
    ASSERT_TRUE(store.Update("cam-stress", {static_cast<std::int64_t>(i), i, i}));
  }
  const auto got = store.Get("cam-stress");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->queue_depth, 499u);
}

TEST(RedisHotStateStore, ConstructorThrowsWhenNothingIsListening) {
  ridgeline::RedisConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = 1;  // Reserved/unlikely to have anything bound; no RealRedisServer started for this test.
  cfg.connect_timeout_ms = 500;
  EXPECT_THROW(ridgeline::RedisHotStateStore{cfg}, std::runtime_error);
}

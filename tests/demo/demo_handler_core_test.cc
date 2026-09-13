// Tests HandleDemoRequest's actual behavior with zero real network and no
// Lambda runtime at all -- the same "fake transport, real logic" pattern
// as weather_client_test.cc and device_shadow_store_test.cc.

#include "ridgeline/demo_handler_core.h"

#include <gtest/gtest.h>

namespace {

class FakeWeatherTransport : public ridgeline::HttpGetTransport {
 public:
  void QueueResponse(int status, const std::string& body) { queued_.push_back({status, body}); }
  ridgeline::HttpGetResponse Get(const std::string&) override {
    if (queued_.empty()) return {0, ""};
    auto r = queued_.front();
    queued_.erase(queued_.begin());
    return r;
  }

 private:
  std::vector<ridgeline::HttpGetResponse> queued_;
};

class FakeDynamoTransport : public ridgeline::HttpTransport {
 public:
  void QueueResponse(int status, const std::string& body) { queued_.push_back({status, body}); }
  int put_count() const { return put_count_; }
  ridgeline::HttpResponse Post(const std::string&, const std::string&, const std::map<std::string, std::string>&) override {
    ++put_count_;
    if (queued_.empty()) return {0, ""};
    auto r = queued_.front();
    queued_.erase(queued_.begin());
    return r;
  }

 private:
  std::vector<ridgeline::HttpResponse> queued_;
  int put_count_ = 0;
};

constexpr const char* kValidWeather = R"({
  "current": {
    "temperature_2m": 22.0,
    "relative_humidity_2m": 40,
    "wind_speed_10m": 10.0,
    "wind_direction_10m": 180
  }
})";

ridgeline::SigV4Credentials TestCreds() { return {"fake-key", "fake-secret", "us-west-2", "dynamodb"}; }

}  // namespace

TEST(DemoHandlerCore, HighConfidenceCalmWeatherGivesHighSeverity) {
  auto weather_transport = std::make_shared<FakeWeatherTransport>();
  weather_transport->QueueResponse(200, kValidWeather);
  ridgeline::WeatherClient weather_client(weather_transport);

  auto dynamo_transport = std::make_shared<FakeDynamoTransport>();
  dynamo_transport->QueueResponse(200, "{}");
  ridgeline::demo::DynamoDemoWriter writer(dynamo_transport, TestCreds(), "https://fake.example.com", "demo_events", "");

  ridgeline::demo::DemoRequest req;
  req.confidence = 0.9f;
  const auto resp = ridgeline::demo::HandleDemoRequest(req, weather_client, &writer, 1735689600000000000LL);

  EXPECT_EQ(resp.severity, ridgeline::AlertSeverity::kHigh);
  EXPECT_TRUE(resp.weather_available);
  EXPECT_DOUBLE_EQ(resp.temperature_c, 22.0);
  EXPECT_TRUE(resp.recorded);
  EXPECT_EQ(dynamo_transport->put_count(), 1);
}

TEST(DemoHandlerCore, WeatherFetchFailureDegradesGracefullyNotSilently) {
  auto weather_transport = std::make_shared<FakeWeatherTransport>();
  weather_transport->QueueResponse(500, "error");  // Live API unreachable/erroring.
  ridgeline::WeatherClient weather_client(weather_transport);

  ridgeline::demo::DemoRequest req;
  req.confidence = 0.9f;
  const auto resp = ridgeline::demo::HandleDemoRequest(req, weather_client, /*writer=*/nullptr, 0);

  EXPECT_FALSE(resp.weather_available);
  // The core correctness property from ADR-0014, exercised end to end
  // through this handler: missing weather must never suppress an alert.
  EXPECT_NE(resp.severity, ridgeline::AlertSeverity::kNone);
  EXPECT_EQ(resp.severity, ridgeline::AlertSeverity::kHigh);  // Confidence-only tier, no weather escalation possible.
}

TEST(DemoHandlerCore, ExtremeWindEscalatesToCriticalThroughTheFullPath) {
  auto weather_transport = std::make_shared<FakeWeatherTransport>();
  weather_transport->QueueResponse(200, R"({"current": {"temperature_2m": 30, "relative_humidity_2m": 50,
                                            "wind_speed_10m": 70.0, "wind_direction_10m": 90}})");
  ridgeline::WeatherClient weather_client(weather_transport);

  ridgeline::demo::DemoRequest req;
  req.confidence = 0.5f;
  const auto resp = ridgeline::demo::HandleDemoRequest(req, weather_client, nullptr, 0);

  EXPECT_EQ(resp.severity, ridgeline::AlertSeverity::kCritical);
}

TEST(DemoHandlerCore, NullWriterSkipsRecordingWithoutCrashing) {
  auto weather_transport = std::make_shared<FakeWeatherTransport>();
  weather_transport->QueueResponse(200, kValidWeather);
  ridgeline::WeatherClient weather_client(weather_transport);

  ridgeline::demo::DemoRequest req;
  const auto resp = ridgeline::demo::HandleDemoRequest(req, weather_client, nullptr, 0);
  EXPECT_FALSE(resp.recorded);
}

TEST(DemoHandlerCore, DynamoWriteFailureIsReflectedButDoesNotAffectSeverity) {
  auto weather_transport = std::make_shared<FakeWeatherTransport>();
  weather_transport->QueueResponse(200, kValidWeather);
  ridgeline::WeatherClient weather_client(weather_transport);

  auto dynamo_transport = std::make_shared<FakeDynamoTransport>();
  dynamo_transport->QueueResponse(500, "throttled");  // DynamoDB write fails.
  ridgeline::demo::DynamoDemoWriter writer(dynamo_transport, TestCreds(), "https://fake.example.com", "demo_events", "");

  ridgeline::demo::DemoRequest req;
  req.confidence = 0.9f;
  const auto resp = ridgeline::demo::HandleDemoRequest(req, weather_client, &writer, 0);

  EXPECT_FALSE(resp.recorded);
  EXPECT_EQ(resp.severity, ridgeline::AlertSeverity::kHigh)
      << "a DynamoDB write failure must never change the severity result the visitor actually sees";
}

TEST(DemoHandlerCore, SessionTokenIsIncludedWhenProvided) {
  auto dynamo_transport = std::make_shared<FakeDynamoTransport>();
  dynamo_transport->QueueResponse(200, "{}");
  // Constructing with a non-empty session token must not throw or
  // otherwise fail -- the real assertion here is just that Lambda's
  // three-credential-part temporary identity is accepted at all, since a
  // static-credentials-only implementation would be unusable inside a
  // real Lambda execution role.
  ridgeline::demo::DynamoDemoWriter writer(dynamo_transport, TestCreds(), "https://fake.example.com", "demo_events",
                                          "AQoDYXdzEJr...fake-session-token");
  ridgeline::demo::DemoEvent event;
  event.event_id = "test-1";
  EXPECT_TRUE(writer.PutEvent(event));
}

TEST(DemoHandlerCore, ListRecentParsesRealScanResponseShape) {
  auto dynamo_transport = std::make_shared<FakeDynamoTransport>();
  const std::string scan_response = R"({
    "Items": [
      {"event_id": {"S": "200"}, "timestamp_unix_ns": {"N": "200"}, "confidence": {"N": "0.5"},
       "severity": {"S": "medium"}, "temperature_c": {"N": "20"}, "relative_humidity_pct": {"N": "50"},
       "wind_speed_kmh": {"N": "5"}, "weather_available": {"BOOL": true}, "user_triggered": {"BOOL": false}},
      {"event_id": {"S": "100"}, "timestamp_unix_ns": {"N": "100"}, "confidence": {"N": "0.9"},
       "severity": {"S": "high"}, "temperature_c": {"N": "18"}, "relative_humidity_pct": {"N": "40"},
       "wind_speed_kmh": {"N": "10"}, "weather_available": {"BOOL": true}, "user_triggered": {"BOOL": true}}
    ]
  })";
  dynamo_transport->QueueResponse(200, scan_response);
  ridgeline::demo::DynamoDemoWriter writer(dynamo_transport, TestCreds(), "https://fake.example.com", "demo_events", "");

  const auto events = writer.ListRecent(20);
  ASSERT_EQ(events.size(), 2u);
  // Sorted newest-first regardless of the order Scan happened to return them in.
  EXPECT_EQ(events[0].event_id, "200");
  EXPECT_EQ(events[0].severity, "medium");
  EXPECT_EQ(events[1].event_id, "100");
  EXPECT_TRUE(events[1].user_triggered);
  EXPECT_FALSE(events[0].user_triggered);
}

TEST(DemoHandlerCore, ListRecentReturnsEmptyOnFailureRatherThanCrashing) {
  auto dynamo_transport = std::make_shared<FakeDynamoTransport>();
  dynamo_transport->QueueResponse(500, "error");
  ridgeline::demo::DynamoDemoWriter writer(dynamo_transport, TestCreds(), "https://fake.example.com", "demo_events", "");
  EXPECT_TRUE(writer.ListRecent(20).empty());
}

TEST(DemoHandlerCore, ListRecentSkipsMalformedRowsWithoutAbortingTheWholeList) {
  auto dynamo_transport = std::make_shared<FakeDynamoTransport>();
  const std::string scan_response = R"({
    "Items": [
      {"event_id": {"S": "1"}, "timestamp_unix_ns": {"N": "1"}, "confidence": {"N": "0.5"},
       "severity": {"S": "low"}, "temperature_c": {"N": "20"}, "relative_humidity_pct": {"N": "50"},
       "wind_speed_kmh": {"N": "5"}, "weather_available": {"BOOL": true}, "user_triggered": {"BOOL": false}},
      {"malformed": "row with no event_id or severity at all"}
    ]
  })";
  dynamo_transport->QueueResponse(200, scan_response);
  ridgeline::demo::DynamoDemoWriter writer(dynamo_transport, TestCreds(), "https://fake.example.com", "demo_events", "");
  const auto events = writer.ListRecent(20);
  ASSERT_EQ(events.size(), 1u) << "the one well-formed row should still come through despite the malformed one";
  EXPECT_EQ(events[0].event_id, "1");
}

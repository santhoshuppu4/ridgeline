// Tests WeatherClient's request/parsing logic entirely without a network
// or a real weather API, by injecting FakeHttpGetTransport -- see
// weather_client.h's top comment: a live curl from this sandbox to
// api.open-meteo.com was confirmed to fail (host not in the sandbox's
// network allowlist), so the real round trip can only be verified on a
// machine with normal internet access. The live integration is in
// scripts/weather_test.sh, meant to run there.

#include "ridgeline/weather_client.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

class FakeHttpGetTransport : public ridgeline::HttpGetTransport {
 public:
  void QueueResponse(int status, const std::string& body) { queued_.push_back({status, body}); }

  ridgeline::HttpGetResponse Get(const std::string& url) override {
    requested_urls_.push_back(url);
    if (queued_.empty()) return {0, ""};
    ridgeline::HttpGetResponse resp = queued_.front();
    queued_.erase(queued_.begin());
    return resp;
  }

  const std::vector<std::string>& requested_urls() const { return requested_urls_; }

 private:
  std::vector<ridgeline::HttpGetResponse> queued_;
  std::vector<std::string> requested_urls_;
};

// A response shape matching Open-Meteo's documented current-conditions
// endpoint. Kept as a literal here rather than round-tripped through the
// production code's own JSON construction -- a test that builds its input
// with the exact same code path it's testing can pass even if that shared
// code is wrong in the same way twice.
constexpr const char* kValidResponse = R"({
  "latitude": 34.05,
  "longitude": -118.24,
  "current": {
    "time": "2026-01-01T00:00",
    "temperature_2m": 18.5,
    "relative_humidity_2m": 42,
    "wind_speed_10m": 12.3,
    "wind_direction_10m": 270
  }
})";

}  // namespace

TEST(WeatherClient, ParsesAValidResponseCorrectly) {
  auto transport = std::make_shared<FakeHttpGetTransport>();
  transport->QueueResponse(200, kValidResponse);
  ridgeline::WeatherClient client(transport, "https://fake.example.com/forecast");

  const auto result = client.FetchCurrent(34.05, -118.24);
  ASSERT_TRUE(result.has_value());
  EXPECT_DOUBLE_EQ(result->temperature_c, 18.5);
  EXPECT_DOUBLE_EQ(result->relative_humidity_pct, 42.0);
  EXPECT_DOUBLE_EQ(result->wind_speed_kmh, 12.3);
  EXPECT_DOUBLE_EQ(result->wind_direction_deg, 270.0);
  EXPECT_GT(result->fetched_unix_ns, 0);
}

TEST(WeatherClient, RequestUrlIncludesLatitudeAndLongitude) {
  auto transport = std::make_shared<FakeHttpGetTransport>();
  transport->QueueResponse(200, kValidResponse);
  ridgeline::WeatherClient client(transport, "https://fake.example.com/forecast");

  client.FetchCurrent(34.05, -118.24);
  ASSERT_EQ(transport->requested_urls().size(), 1u);
  const auto& url = transport->requested_urls()[0];
  EXPECT_NE(url.find("latitude=34.05"), std::string::npos);
  EXPECT_NE(url.find("longitude=-118.24"), std::string::npos);
}

TEST(WeatherClient, ReturnsNulloptOnNonTwoHundredStatus) {
  auto transport = std::make_shared<FakeHttpGetTransport>();
  transport->QueueResponse(500, "internal server error");
  ridgeline::WeatherClient client(transport);
  EXPECT_FALSE(client.FetchCurrent(0, 0).has_value());
}

TEST(WeatherClient, ReturnsNulloptOnConnectionFailure) {
  auto transport = std::make_shared<FakeHttpGetTransport>();
  transport->QueueResponse(0, "");  // status_code 0: transport-level failure.
  ridgeline::WeatherClient client(transport);
  EXPECT_FALSE(client.FetchCurrent(0, 0).has_value());
}

TEST(WeatherClient, ReturnsNulloptOnMalformedJson) {
  auto transport = std::make_shared<FakeHttpGetTransport>();
  transport->QueueResponse(200, "{not valid json");
  ridgeline::WeatherClient client(transport);
  EXPECT_FALSE(client.FetchCurrent(0, 0).has_value());
}

TEST(WeatherClient, ReturnsNulloptWhenCurrentKeyIsMissing) {
  auto transport = std::make_shared<FakeHttpGetTransport>();
  transport->QueueResponse(200, R"({"latitude": 34.05, "longitude": -118.24})");
  ridgeline::WeatherClient client(transport);
  EXPECT_FALSE(client.FetchCurrent(0, 0).has_value())
      << "a response shape change from the provider (e.g. renaming 'current') must fail loudly, not silently";
}

TEST(WeatherClient, ReturnsNulloptWhenAnExpectedFieldIsMissing) {
  auto transport = std::make_shared<FakeHttpGetTransport>();
  transport->QueueResponse(200, R"({"current": {"temperature_2m": 18.5}})");  // Missing humidity/wind fields.
  ridgeline::WeatherClient client(transport);
  EXPECT_FALSE(client.FetchCurrent(0, 0).has_value())
      << "must not return a half-populated WeatherConditions that looks identical to a real, complete reading";
}

TEST(WeatherClient, ReturnsNulloptWhenAFieldHasTheWrongType) {
  auto transport = std::make_shared<FakeHttpGetTransport>();
  transport->QueueResponse(
      200, R"({"current": {"temperature_2m": "not-a-number", "relative_humidity_2m": 42, "wind_speed_10m": 12.3, "wind_direction_10m": 270}})");
  ridgeline::WeatherClient client(transport);
  EXPECT_FALSE(client.FetchCurrent(0, 0).has_value());
}

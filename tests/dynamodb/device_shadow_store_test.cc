// Tests DeviceShadowStore's request/response logic entirely without a
// network or a real DynamoDB endpoint, by injecting FakeHttpTransport --
// see device_shadow_store.h's top comment for why this split exists. The
// live integration test against a real DynamoDB Local endpoint is in
// tests/dynamodb/device_shadow_store_live_test.cc, and is SKIPPED unless
// one is reachable (this project's sandbox has no AWS/Docker network
// access at all; the live test is meant for YOUR machine).

#include "ridgeline/device_shadow_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <vector>

namespace {

using json = nlohmann::json;

// Records every request it receives and returns responses from a
// pre-programmed queue, in order -- lets a test script "the DynamoDB
// server said X" without any of X actually coming over a wire.
class FakeHttpTransport : public ridgeline::HttpTransport {
 public:
  void QueueResponse(int status, const std::string& body) { queued_.push_back({status, body}); }

  ridgeline::HttpResponse Post(const std::string& url, const std::string& body,
                             const std::map<std::string, std::string>& headers) override {
    requests_.push_back({url, body, headers});
    if (queued_.empty()) return {0, ""};  // Test forgot to queue enough responses -- surfaces as a clear failure, not a hang.
    ridgeline::HttpResponse resp = queued_.front();
    queued_.erase(queued_.begin());
    return resp;
  }

  struct RecordedRequest {
    std::string url, body;
    std::map<std::string, std::string> headers;
  };
  const std::vector<RecordedRequest>& requests() const { return requests_; }

 private:
  std::vector<ridgeline::HttpResponse> queued_;
  std::vector<RecordedRequest> requests_;
};

ridgeline::DynamoDbConfig TestConfig() {
  ridgeline::DynamoDbConfig cfg;
  cfg.endpoint = "http://localhost:8000";
  cfg.table_name = "device_shadows";
  cfg.access_key_id = "fakeAccessKeyId";
  cfg.secret_access_key = "fakeSecretAccessKey";
  return cfg;
}

}  // namespace

TEST(DeviceShadowStore, GetReportedParsesSuccessfulResponse) {
  auto transport = std::make_shared<FakeHttpTransport>();
  json response;
  response["Item"] = {{"device_id", {{"S", "cam-1"}}},
                     {"reported_json", {{"S", R"({"fps":15})"}}},
                     {"version", {{"N", "3"}}}};
  transport->QueueResponse(200, response.dump());

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  const auto shadow = store.GetReported("cam-1");

  ASSERT_TRUE(shadow.has_value());
  EXPECT_EQ(shadow->reported_json, R"({"fps":15})");
  EXPECT_EQ(shadow->version, 3);
}

TEST(DeviceShadowStore, GetReportedReturnsNulloptWhenItemMissing) {
  auto transport = std::make_shared<FakeHttpTransport>();
  transport->QueueResponse(200, R"({})");  // DynamoDB's documented shape for "no item at that key".

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  EXPECT_FALSE(store.GetReported("never-seen-device").has_value());
}

TEST(DeviceShadowStore, GetReportedReturnsNulloptOnConnectionError) {
  auto transport = std::make_shared<FakeHttpTransport>();
  transport->QueueResponse(0, "");  // status_code 0: transport-level failure, no HTTP response at all.

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  EXPECT_FALSE(store.GetReported("cam-1").has_value());
}

TEST(DeviceShadowStore, GetReportedReturnsNulloptOnMalformedJsonBody) {
  auto transport = std::make_shared<FakeHttpTransport>();
  transport->QueueResponse(200, "{not valid json");

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  EXPECT_FALSE(store.GetReported("cam-1").has_value());
}

TEST(DeviceShadowStore, PutReportedSucceedsAndSendsCorrectRequestShape) {
  auto transport = std::make_shared<FakeHttpTransport>();
  transport->QueueResponse(200, "{}");

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  const auto result = store.PutReported("cam-1", R"({"fps":30})", /*expected_version=*/5);

  EXPECT_EQ(result, ridgeline::PutResult::kSuccess);
  ASSERT_EQ(transport->requests().size(), 1u);

  const json sent = json::parse(transport->requests()[0].body);
  EXPECT_EQ(sent["TableName"], "device_shadows");
  EXPECT_EQ(sent["Item"]["device_id"]["S"], "cam-1");
  EXPECT_EQ(sent["Item"]["reported_json"]["S"], R"({"fps":30})");
  EXPECT_EQ(sent["Item"]["version"]["N"], "6");  // expected_version + 1: the write always advances the version.
  EXPECT_EQ(sent["ConditionExpression"], "version = :v");
  EXPECT_EQ(sent["ExpressionAttributeValues"][":v"]["N"], "5");

  const auto& headers = transport->requests()[0].headers;
  ASSERT_TRUE(headers.count("authorization"));
  EXPECT_NE(headers.at("authorization").find("AWS4-HMAC-SHA256"), std::string::npos);
  EXPECT_EQ(headers.at("x-amz-target"), "DynamoDB_20120810.PutItem");
}

TEST(DeviceShadowStore, PutReportedWithZeroExpectedVersionUsesCreateCondition) {
  auto transport = std::make_shared<FakeHttpTransport>();
  transport->QueueResponse(200, "{}");

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  store.PutReported("brand-new-device", R"({"fps":15})", /*expected_version=*/0);

  const json sent = json::parse(transport->requests()[0].body);
  EXPECT_EQ(sent["ConditionExpression"], "attribute_not_exists(device_id)")
      << "version=0 must mean 'this device must not already have a shadow', not 'version literally equals 0'";
  EXPECT_FALSE(sent.contains("ExpressionAttributeValues"));
}

TEST(DeviceShadowStore, PutReportedDetectsVersionConflictSpecifically) {
  auto transport = std::make_shared<FakeHttpTransport>();
  json error_body;
  error_body["__type"] = "com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException";
  error_body["message"] = "The conditional request failed";
  transport->QueueResponse(400, error_body.dump());

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  const auto result = store.PutReported("cam-1", R"({"fps":30})", /*expected_version=*/5);

  EXPECT_EQ(result, ridgeline::PutResult::kVersionConflict)
      << "a conditional-check failure must be distinguishable from a generic error, so a caller can "
         "decide whether to re-read and retry vs. give up";
}

TEST(DeviceShadowStore, PutReportedReturnsGenericErrorForOtherFourHundreds) {
  auto transport = std::make_shared<FakeHttpTransport>();
  json error_body;
  error_body["__type"] = "com.amazonaws.dynamodb.v20120810#ResourceNotFoundException";
  transport->QueueResponse(400, error_body.dump());

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  EXPECT_EQ(store.PutReported("cam-1", "{}", 0), ridgeline::PutResult::kError);
}

TEST(DeviceShadowStore, PutReportedReturnsErrorOnConnectionFailureNotVersionConflict) {
  auto transport = std::make_shared<FakeHttpTransport>();
  transport->QueueResponse(0, "");

  ridgeline::DeviceShadowStore store(TestConfig(), transport);
  EXPECT_EQ(store.PutReported("cam-1", "{}", 0), ridgeline::PutResult::kError)
      << "a dropped connection must never be misreported as a version conflict -- the caller needs to "
         "know 'try again later' vs. 're-read, someone else won'";
}

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "ridgeline/sigv4_signer.h"

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHAT THIS IS FOR: the durable "device shadow" -- desired vs. reported
// config per device -- as opposed to RedisHotStateStore's ephemeral cache.
// If this is lost, real information is gone (the last known reported
// config), which is why it's DynamoDB, not Redis.
//
// SCHEMA (single-table, per the original design doc): partition key
// device_id (string), attributes reported_json (string), version (number).
// `version` backs OPTIMISTIC CONCURRENCY: PutReported takes an
// expected_version and issues a conditional PutItem
// (`ConditionExpression: version = :expected`), so two writers racing to
// update the same device's shadow can't silently clobber each other --
// the loser gets a definite "someone else wrote first" signal
// (kVersionConflict) instead of a corrupted or stale-overwritten record.
//
// WHY AN INJECTABLE HttpTransport INTERFACE INSTEAD OF CALLING libcurl
// DIRECTLY: the retry/error-classification logic (throttling vs. conditional
// check failure vs. genuine network error) is exactly the part worth testing
// thoroughly, and none of it actually needs a real HTTP round trip to
// verify. Coding against an interface -- the same "depend on an interface"
// idea as Detector and DeviceHotState's design -- means
// tests/dynamodb/device_shadow_store_test.cc can inject a FakeHttpTransport
// that returns canned responses (200 success, 400
// ConditionalCheckFailedException, 400 ProvisionedThroughputExceededException,
// a connection error) and assert the CORRECT resulting behavior, with zero
// network, zero DynamoDB Local, and zero flakiness. The real
// CurlHttpTransport is exercised only by the live integration test, which is
// explicitly SKIPPED when no endpoint is reachable -- see
// context/adr/0008 for why that split exists and what it does and doesn't
// prove.
// ---------------------------------------------------------------------------

struct HttpResponse {
  int status_code = 0;  // 0 means "no response at all" (connection error, timeout).
  std::string body;
};

// Abstraction over "send a signed POST and get a response back." Implemented
// for real by CurlDynamoDbTransport (dynamodb/src/curl_http_transport.cc);
// implemented for tests by an in-memory fake with no network at all.
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse Post(const std::string& url, const std::string& body,
                           const std::map<std::string, std::string>& headers) = 0;
};

struct DynamoDbConfig {
  std::string endpoint = "http://localhost:8000";  // DynamoDB Local's default; a real AWS endpoint works identically.
  std::string table_name = "device_shadows";
  std::string region = "us-west-2";
  // DynamoDB Local accepts any non-empty access key / secret; a real
  // deployment would source these from the environment, never hardcode
  // them -- deliberately not defaulted to a placeholder here, to keep that
  // responsibility visible at the call site rather than silently baked in.
  std::string access_key_id;
  std::string secret_access_key;
  int timeout_ms = 3000;
};

enum class PutResult { kSuccess, kVersionConflict, kError };

struct DeviceShadow {
  std::string reported_json;
  std::int64_t version = 0;
};

class DeviceShadowStore {
 public:
  // `transport` ownership is shared with the caller's choice: production
  // code constructs with a real CurlDynamoDbTransport; tests construct with
  // a FakeHttpTransport. Either way this class only ever calls the
  // interface, never libcurl directly.
  DeviceShadowStore(DynamoDbConfig config, std::shared_ptr<HttpTransport> transport);

  // Returns nullopt if the device has no shadow yet, or on any error --
  // logged via the returned bool from GetLastErrorWasTransport(), not
  // thrown, since "no shadow yet" and "DynamoDB is briefly unreachable" are
  // both routine, expected conditions for a caller to handle by proceeding
  // without shadow data, not by crashing.
  std::optional<DeviceShadow> GetReported(const std::string& device_id);

  // Conditional write: succeeds only if the item's current version equals
  // expected_version (0 means "must not exist yet" -- DynamoDB's
  // attribute_not_exists condition). On kVersionConflict, the caller should
  // re-read via GetReported and decide whether to retry with the new
  // version or abandon the write -- retrying automatically inside this
  // function would hide a real conflict from the caller that might care.
  PutResult PutReported(const std::string& device_id, const std::string& reported_json, std::int64_t expected_version);

 private:
  HttpResponse SignedPost(const std::string& target, const std::string& json_body);

  DynamoDbConfig config_;
  std::shared_ptr<HttpTransport> transport_;
  SigV4Signer signer_;
};

}  // namespace ridgeline

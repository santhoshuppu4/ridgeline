#include "ridgeline/device_shadow_store.h"

#include <chrono>
#include <nlohmann/json.hpp>

namespace ridgeline {

using json = nlohmann::json;

namespace {
std::string HostFromEndpoint(const std::string& endpoint) {
  // "http://localhost:8000" -> "localhost:8000". DynamoDB's SigV4 signing
  // requires the bare host[:port], not the scheme -- a common first-try bug
  // is signing with the scheme still attached, which produces a
  // well-formed but wrong signature (same failure shape the sigv4_signer.h
  // comment warns about for the raw/hex HMAC mixup).
  auto pos = endpoint.find("://");
  return pos == std::string::npos ? endpoint : endpoint.substr(pos + 3);
}
}  // namespace

DeviceShadowStore::DeviceShadowStore(DynamoDbConfig config, std::shared_ptr<HttpTransport> transport)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      signer_(SigV4Credentials{config_.access_key_id, config_.secret_access_key, config_.region, "dynamodb"}) {}

HttpResponse DeviceShadowStore::SignedPost(const std::string& target, const std::string& json_body) {
  const std::string host = HostFromEndpoint(config_.endpoint);
  const auto unix_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  const std::map<std::string, std::string> headers_to_sign = {
      {"content-type", "application/x-amz-json-1.0"},
      {"x-amz-target", "DynamoDB_20120810." + target},
  };
  const auto signed_req = signer_.Sign("POST", host, "/", json_body, headers_to_sign, unix_seconds);

  std::map<std::string, std::string> headers = headers_to_sign;
  headers["x-amz-date"] = signed_req.amz_date;
  headers["authorization"] = signed_req.authorization_header;
  return transport_->Post(config_.endpoint, json_body, headers);
}

std::optional<DeviceShadow> DeviceShadowStore::GetReported(const std::string& device_id) {
  json req;
  req["TableName"] = config_.table_name;
  req["Key"] = {{"device_id", {{"S", device_id}}}};

  const HttpResponse resp = SignedPost("GetItem", req.dump());
  if (resp.status_code != 200) return std::nullopt;  // Includes 400 (e.g. ResourceNotFoundException) and connection errors.

  json body;
  try {
    body = json::parse(resp.body);
  } catch (const json::parse_error&) {
    return std::nullopt;  // Malformed response body: treat like any other unusable read, don't crash on it.
  }
  if (!body.contains("Item")) return std::nullopt;  // GetItem's documented shape for "no such key".

  DeviceShadow shadow;
  try {
    shadow.reported_json = body["Item"]["reported_json"]["S"].get<std::string>();
    shadow.version = std::stoll(body["Item"]["version"]["N"].get<std::string>());
  } catch (const std::exception&) {
    return std::nullopt;  // Response present but not in the expected shape -- same "unusable read" treatment.
  }
  return shadow;
}

PutResult DeviceShadowStore::PutReported(const std::string& device_id, const std::string& reported_json,
                                        std::int64_t expected_version) {
  json req;
  req["TableName"] = config_.table_name;
  req["Item"] = {{"device_id", {{"S", device_id}}},
                {"reported_json", {{"S", reported_json}}},
                {"version", {{"N", std::to_string(expected_version + 1)}}}};

  if (expected_version == 0) {
    // "Must not already exist" -- the create case.
    req["ConditionExpression"] = "attribute_not_exists(device_id)";
  } else {
    // "Must currently be exactly this version" -- the optimistic-concurrency
    // update case. Uses an expression attribute value (":v") rather than
    // inlining the number directly in the condition string, which is the
    // DynamoDB-documented way to avoid the condition-expression parser
    // treating part of a value as syntax.
    req["ConditionExpression"] = "version = :v";
    req["ExpressionAttributeValues"] = {{":v", {{"N", std::to_string(expected_version)}}}};
  }

  const HttpResponse resp = SignedPost("PutItem", req.dump());
  if (resp.status_code == 200) return PutResult::kSuccess;

  if (resp.status_code == 400) {
    try {
      const json body = json::parse(resp.body);
      const std::string type = body.value("__type", "");
      if (type.find("ConditionalCheckFailedException") != std::string::npos) {
        return PutResult::kVersionConflict;
      }
    } catch (const json::parse_error&) {
      // Fall through to kError: a 400 we can't even parse is still an error,
      // just not one we can specifically identify as a version conflict.
    }
  }
  return PutResult::kError;
}

}  // namespace ridgeline

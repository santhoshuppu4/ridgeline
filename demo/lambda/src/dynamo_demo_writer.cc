#include "ridgeline/dynamo_demo_writer.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <sstream>

namespace ridgeline::demo {

using json = nlohmann::json;

DynamoDemoWriter::DynamoDemoWriter(std::shared_ptr<HttpTransport> transport, SigV4Credentials credentials,
                                   std::string endpoint, std::string table_name, std::string session_token)
    : transport_(std::move(transport)),
      signer_(std::move(credentials)),
      endpoint_(std::move(endpoint)),
      table_name_(std::move(table_name)),
      session_token_(std::move(session_token)) {}

namespace {
std::string HostFromEndpoint(const std::string& endpoint) {
  auto pos = endpoint.find("://");
  return pos == std::string::npos ? endpoint : endpoint.substr(pos + 3);
}
}  // namespace

bool DynamoDemoWriter::PutEvent(const DemoEvent& event) {
  json item;
  item["event_id"] = {{"S", event.event_id}};
  item["timestamp_unix_ns"] = {{"N", std::to_string(event.timestamp_unix_ns)}};
  item["confidence"] = {{"N", std::to_string(event.confidence)}};
  item["severity"] = {{"S", event.severity}};
  item["temperature_c"] = {{"N", std::to_string(event.temperature_c)}};
  item["relative_humidity_pct"] = {{"N", std::to_string(event.relative_humidity_pct)}};
  item["wind_speed_kmh"] = {{"N", std::to_string(event.wind_speed_kmh)}};
  item["weather_available"] = {{"BOOL", event.weather_available}};
  item["user_triggered"] = {{"BOOL", event.user_triggered}};

  json req;
  req["TableName"] = table_name_;
  req["Item"] = item;
  const std::string body = req.dump();

  const std::string host = HostFromEndpoint(endpoint_);
  const auto unix_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  // x-amz-security-token: the one extension this demo needed beyond what
  // SigV4Signer originally shipped with -- see this header's top comment.
  // Passed as an ordinary extra_header; SigV4Signer signs whatever it's
  // given, so no change to SigV4Signer itself was required.
  std::map<std::string, std::string> headers_to_sign = {
      {"content-type", "application/x-amz-json-1.0"},
      {"x-amz-target", "DynamoDB_20120810.PutItem"},
  };
  if (!session_token_.empty()) headers_to_sign["x-amz-security-token"] = session_token_;

  const auto signed_req = signer_.Sign("POST", host, "/", body, headers_to_sign, unix_seconds);

  std::map<std::string, std::string> headers = headers_to_sign;
  headers["x-amz-date"] = signed_req.amz_date;
  headers["authorization"] = signed_req.authorization_header;

  const HttpResponse resp = transport_->Post(endpoint_, body, headers);
  return resp.status_code == 200;
}

namespace {
double ParseN(const json& attr_map, const char* key, double fallback = 0.0) {
  try {
    return std::stod(attr_map.at(key).at("N").get<std::string>());
  } catch (const std::exception&) {
    return fallback;  // Malformed/missing attribute: treat like any other "unusable field" in this project
                      // (see device_shadow_store.cc's identical reasoning) -- don't let one bad row abort the whole list.
  }
}
}  // namespace

std::vector<DemoEvent> DynamoDemoWriter::ListRecent(int limit) {
  std::vector<DemoEvent> out;

  json req;
  req["TableName"] = table_name_;
  req["Limit"] = limit;
  const std::string body = req.dump();

  const std::string host = HostFromEndpoint(endpoint_);
  const auto unix_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  std::map<std::string, std::string> headers_to_sign = {
      {"content-type", "application/x-amz-json-1.0"},
      {"x-amz-target", "DynamoDB_20120810.Scan"},
  };
  if (!session_token_.empty()) headers_to_sign["x-amz-security-token"] = session_token_;

  const auto signed_req = signer_.Sign("POST", host, "/", body, headers_to_sign, unix_seconds);
  std::map<std::string, std::string> headers = headers_to_sign;
  headers["x-amz-date"] = signed_req.amz_date;
  headers["authorization"] = signed_req.authorization_header;

  const HttpResponse resp = transport_->Post(endpoint_, body, headers);
  if (resp.status_code != 200) return out;  // Best-effort: see this method's header comment.

  json parsed;
  try {
    parsed = json::parse(resp.body);
  } catch (const json::parse_error&) {
    return out;
  }
  if (!parsed.contains("Items") || !parsed["Items"].is_array()) return out;

  for (const auto& item : parsed["Items"]) {
    DemoEvent e;
    try {
      e.event_id = item.at("event_id").at("S").get<std::string>();
      e.severity = item.at("severity").at("S").get<std::string>();
      e.weather_available = item.value("weather_available", json{}).value("BOOL", false);
      e.user_triggered = item.value("user_triggered", json{}).value("BOOL", false);
    } catch (const std::exception&) {
      continue;  // A malformed row is skipped, not fatal to the whole list -- same "one bad row doesn't ruin
                // everything" reasoning as ParseN above.
    }
    e.timestamp_unix_ns = static_cast<std::int64_t>(ParseN(item, "timestamp_unix_ns"));
    e.confidence = static_cast<float>(ParseN(item, "confidence"));
    e.temperature_c = ParseN(item, "temperature_c");
    e.relative_humidity_pct = ParseN(item, "relative_humidity_pct");
    e.wind_speed_kmh = ParseN(item, "wind_speed_kmh");
    out.push_back(std::move(e));
  }

  std::sort(out.begin(), out.end(),
           [](const DemoEvent& a, const DemoEvent& b) { return a.timestamp_unix_ns > b.timestamp_unix_ns; });
  return out;
}

}  // namespace ridgeline::demo

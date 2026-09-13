// Thin Lambda entry point. Deliberately small: parses the incoming event
// (API Gateway proxy-integration JSON for a visitor-triggered request, or
// an EventBridge scheduled event for synthetic "live" traffic), builds a
// real WeatherClient/DynamoDemoWriter with credentials from Lambda's
// environment, and delegates everything else to
// ridgeline::demo::HandleDemoRequest -- see demo_handler_core.h's top
// comment for why the split is drawn here.
//
// NOT VERIFIED IN THIS SANDBOX: this file can only be compiled here (no
// real AWS Lambda runtime exists in this development environment to
// actually invoke it against). See demo/README.md for the deploy and
// smoke-test steps meant to run on real infrastructure.

#include <aws/lambda-runtime/runtime.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <memory>

#include "ridgeline/curl_http_transport.h"
#include "ridgeline/curl_get_transport.h"
#include "ridgeline/demo_handler_core.h"
#include "ridgeline/time.h"

using namespace aws::lambda_runtime;
using json = nlohmann::json;

namespace {

std::string EnvOrEmpty(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

invocation_response HandleInvocation(invocation_request const& req) {
  json event;
  try {
    event = json::parse(req.payload);
  } catch (const json::parse_error&) {
    return invocation_response::failure("could not parse event JSON", "InvalidEvent");
  }

  ridgeline::SigV4Credentials creds{EnvOrEmpty("AWS_ACCESS_KEY_ID"), EnvOrEmpty("AWS_SECRET_ACCESS_KEY"),
                                   EnvOrEmpty("AWS_REGION"), "dynamodb"};
  ridgeline::demo::DynamoDemoWriter writer(std::make_shared<ridgeline::CurlHttpTransport>(), creds,
                                          "https://dynamodb." + EnvOrEmpty("AWS_REGION") + ".amazonaws.com",
                                          EnvOrEmpty("DEMO_TABLE_NAME"), EnvOrEmpty("AWS_SESSION_TOKEN"));

  // API Gateway HTTP API (payload format 2.0) puts the method here; a
  // scheduled EventBridge event has no "requestContext" at all, which is
  // how the three cases (list / trigger / scheduled) are told apart.
  std::string method;
  if (event.contains("requestContext") && event["requestContext"].contains("http")) {
    method = event["requestContext"]["http"].value("method", "");
  }

  json api_response;
  api_response["headers"] = {{"Content-Type", "application/json"}, {"Access-Control-Allow-Origin", "*"}};

  if (method == "GET") {
    // "Try it yourself" page load / live-feed poll: return recent events,
    // no severity computation needed for this branch at all.
    const auto events = writer.ListRecent(20);
    json arr = json::array();
    for (const auto& e : events) {
      arr.push_back({{"event_id", e.event_id},
                     {"timestamp_unix_ns", e.timestamp_unix_ns},
                     {"confidence", e.confidence},
                     {"severity", e.severity},
                     {"temperature_c", e.temperature_c},
                     {"relative_humidity_pct", e.relative_humidity_pct},
                     {"wind_speed_kmh", e.wind_speed_kmh},
                     {"weather_available", e.weather_available},
                     {"user_triggered", e.user_triggered}});
    }
    api_response["statusCode"] = 200;
    api_response["body"] = json{{"events", arr}}.dump();
    return invocation_response::success(api_response.dump(), "application/json");
  }

  ridgeline::demo::DemoRequest demo_req;
  // A POST body (visitor-triggered via the "try it yourself" button) is a
  // JSON *string* under "body" in the API Gateway proxy format; an
  // EventBridge scheduled event has no "body" at all.
  if (event.contains("body") && event["body"].is_string()) {
    demo_req.user_triggered = true;
    try {
      const json body = json::parse(event["body"].get<std::string>());
      if (body.contains("confidence")) demo_req.confidence = body["confidence"].get<float>();
    } catch (const json::parse_error&) {
      return invocation_response::failure("could not parse request body", "InvalidBody");
    }
  } else {
    // Scheduled synthetic traffic: a fixed, illustrative confidence value.
    // Not randomized -- a visitor comparing two "live feed" entries side by
    // side should see a real, reproducible weather-driven difference
    // between them, not noise from an arbitrary random confidence draw.
    demo_req.user_triggered = false;
    demo_req.confidence = 0.72f;
  }

  ridgeline::WeatherClient weather_client(std::make_shared<ridgeline::CurlGetTransport>());
  const auto result =
      ridgeline::demo::HandleDemoRequest(demo_req, weather_client, &writer, ridgeline::NowUnixNs());

  json out;
  out["severity"] = ridgeline::ToString(result.severity);
  out["weather_available"] = result.weather_available;
  out["temperature_c"] = result.temperature_c;
  out["relative_humidity_pct"] = result.relative_humidity_pct;
  out["wind_speed_kmh"] = result.wind_speed_kmh;
  out["recorded"] = result.recorded;

  api_response["statusCode"] = 200;
  api_response["body"] = out.dump();
  return invocation_response::success(api_response.dump(), "application/json");
}

}  // namespace

int main() {
  run_handler(HandleInvocation);
  return 0;
}

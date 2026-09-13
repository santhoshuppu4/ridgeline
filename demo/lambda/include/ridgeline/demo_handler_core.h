#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ridgeline/alert_engine.h"
#include "ridgeline/dynamo_demo_writer.h"
#include "ridgeline/weather_client.h"

namespace ridgeline::demo {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHY THIS IS SEPARATE FROM handler.cc: this is the ENTIRE actual behavior
// of the demo -- fetch weather, fuse it with a confidence value using the
// real ComputeAlertSeverity, record the result, return it -- with zero
// dependency on the Lambda Runtime API. handler.cc (the actual
// aws-lambda-cpp entry point) is a thin ~20-line wrapper that parses the
// Lambda event JSON and calls this. That split means the interesting
// logic is fully unit-testable right here, with fake weather/DynamoDB
// transports, and handler.cc has so little in it that it barely needs
// testing at all -- the same "keep the untestable-here part thin" idea
// behind every real-vs-fake transport split in this project.
// ---------------------------------------------------------------------------

struct DemoRequest {
  float confidence = 0.0f;
  double latitude = 34.05;   // Defaults: Los Angeles -- picked for the same reason as ADR-0014's live test:
  double longitude = -118.24;  // real, variable weather, not a special significance to the location itself.
  bool user_triggered = false;
};

struct DemoResponse {
  AlertSeverity severity = AlertSeverity::kNone;
  bool weather_available = false;
  double temperature_c = 0.0;
  double relative_humidity_pct = 0.0;
  double wind_speed_kmh = 0.0;
  bool recorded = false;  // Whether the DynamoDB write succeeded -- best-effort; see DynamoDemoWriter's header comment.
};

// Pure(ish) orchestration: fetch weather (real client, injectable
// transport), compute severity (real, already-tested AlertEngine), attempt
// to record it (best-effort). Never throws; a weather-fetch failure
// degrades gracefully to a confidence-only severity, matching
// ComputeAlertSeverity's own documented contract.
DemoResponse HandleDemoRequest(const DemoRequest& request, WeatherClient& weather_client,
                               DynamoDemoWriter* writer /* nullable: omit to skip recording, e.g. in tests */,
                               std::int64_t now_unix_ns);

}  // namespace ridgeline::demo

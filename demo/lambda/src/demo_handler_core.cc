#include "ridgeline/demo_handler_core.h"

#include <sstream>

namespace ridgeline::demo {

DemoResponse HandleDemoRequest(const DemoRequest& request, WeatherClient& weather_client, DynamoDemoWriter* writer,
                               std::int64_t now_unix_ns) {
  DemoResponse response;

  const auto weather = weather_client.FetchCurrent(request.latitude, request.longitude);
  response.weather_available = weather.has_value();
  if (weather.has_value()) {
    response.temperature_c = weather->temperature_c;
    response.relative_humidity_pct = weather->relative_humidity_pct;
    response.wind_speed_kmh = weather->wind_speed_kmh;
  }

  response.severity = ComputeAlertSeverity(request.confidence, weather);

  if (writer != nullptr) {
    DemoEvent event;
    std::ostringstream id;
    id << now_unix_ns;  // Timestamp-derived ID: simple, monotonically increasing for the demo's low request volume;
                        // a real production system would want something collision-resistant under concurrency.
    event.event_id = id.str();
    event.timestamp_unix_ns = now_unix_ns;
    event.confidence = request.confidence;
    event.severity = ToString(response.severity);
    event.temperature_c = response.temperature_c;
    event.relative_humidity_pct = response.relative_humidity_pct;
    event.wind_speed_kmh = response.wind_speed_kmh;
    event.weather_available = response.weather_available;
    event.user_triggered = request.user_triggered;
    response.recorded = writer->PutEvent(event);
  }

  return response;
}

}  // namespace ridgeline::demo

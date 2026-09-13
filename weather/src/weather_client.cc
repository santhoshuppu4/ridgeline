#include "ridgeline/weather_client.h"

#include <nlohmann/json.hpp>
#include <sstream>

#include "ridgeline/time.h"

namespace ridgeline {

using json = nlohmann::json;

WeatherClient::WeatherClient(std::shared_ptr<HttpGetTransport> transport, std::string base_url)
    : transport_(std::move(transport)), base_url_(std::move(base_url)) {}

std::optional<WeatherConditions> WeatherClient::FetchCurrent(double latitude, double longitude) {
  std::ostringstream url;
  url << base_url_ << "?latitude=" << latitude << "&longitude=" << longitude
      << "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,wind_direction_10m";

  const HttpGetResponse resp = transport_->Get(url.str());
  if (resp.status_code != 200) return std::nullopt;  // Includes connection errors (status_code stays 0) and HTTP errors.

  json body;
  try {
    body = json::parse(resp.body);
  } catch (const json::parse_error&) {
    return std::nullopt;  // Malformed response: an external API is free to change shape without notice.
  }

  if (!body.contains("current")) return std::nullopt;
  const auto& current = body["current"];

  WeatherConditions conditions;
  try {
    conditions.temperature_c = current.at("temperature_2m").get<double>();
    conditions.relative_humidity_pct = current.at("relative_humidity_2m").get<double>();
    conditions.wind_speed_kmh = current.at("wind_speed_10m").get<double>();
    conditions.wind_direction_deg = current.at("wind_direction_10m").get<double>();
  } catch (const json::exception&) {
    // A field genuinely missing, or present with the wrong type -- either
    // way, this is not a reading this code should guess at. Better to
    // report "no weather data" than a WeatherConditions half-populated
    // with a mix of real and default-initialized values that look
    // identical from the caller's side.
    return std::nullopt;
  }
  conditions.fetched_unix_ns = ridgeline::NowUnixNs();
  return conditions;
}

}  // namespace ridgeline

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHY AN INJECTABLE TRANSPORT AGAIN: the same reasoning as
// dynamodb/device_shadow_store.h -- this sandbox has an allowlisted set of
// network domains for testing (verified directly: a live curl to
// api.open-meteo.com here returns "Host not in allowlist," not a timeout or
// a real response), so the actual live API round trip can only be verified
// on a machine with normal internet access. Depending on an interface
// means the PARSING and FUSION logic (the actual engineering content) is
// fully testable right now with a fake transport, and the real transport
// (CurlGetTransport) is a thin, separately-verifiable layer on top.
//
// WHY OPEN-METEO SPECIFICALLY: no API key required (no secret-management
// question to solve for a portfolio project), a plain REST/JSON interface,
// and it's free for non-commercial use. The response shape parsed here
// (`current.temperature_2m`, `.relative_humidity_2m`, `.wind_speed_10m`,
// `.wind_direction_10m`) matches Open-Meteo's documented forecast endpoint
// as of this writing -- but "as of this writing" is exactly the caveat: an
// external API can change its response shape without this project's
// knowledge, which is precisely why WeatherClient::FetchCurrent returns
// nullopt (not a thrown exception, not a best-guess default) on anything
// that doesn't parse as expected, and why the live round trip needs
// verification on real hardware before this is trusted in production. See
// context/adr/0014.
// ---------------------------------------------------------------------------

struct HttpGetResponse {
  int status_code = 0;  // 0 = no response at all (connection error, timeout).
  std::string body;
};

class HttpGetTransport {
 public:
  virtual ~HttpGetTransport() = default;
  virtual HttpGetResponse Get(const std::string& url) = 0;
};

struct WeatherConditions {
  double temperature_c = 0.0;
  double relative_humidity_pct = 0.0;
  double wind_speed_kmh = 0.0;
  double wind_direction_deg = 0.0;
  std::int64_t fetched_unix_ns = 0;
};

class WeatherClient {
 public:
  // `base_url` defaults to Open-Meteo's real forecast endpoint; overridable
  // for tests, or if pointed at a different weather provider with the same
  // response shape.
  WeatherClient(std::shared_ptr<HttpGetTransport> transport,
               std::string base_url = "https://api.open-meteo.com/v1/forecast");

  // Returns nullopt on ANY failure -- unreachable, non-200, malformed JSON,
  // or a response missing an expected field -- rather than a partially
  // populated or best-guess WeatherConditions. A caller (the gateway's
  // alert fusion) must treat "no weather data" as a real, first-class
  // state, not something papered over with zeros that look like valid
  // readings.
  std::optional<WeatherConditions> FetchCurrent(double latitude, double longitude);

 private:
  std::shared_ptr<HttpGetTransport> transport_;
  std::string base_url_;
};

}  // namespace ridgeline

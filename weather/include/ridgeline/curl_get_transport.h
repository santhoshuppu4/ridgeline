#pragma once

#include "ridgeline/weather_client.h"

namespace ridgeline {

// Real network implementation, kept out of weather_client.h/.cc so anything
// that only needs to unit-test WeatherClient's parsing logic never needs
// libcurl's headers -- same "confine the dependency to where it's needed"
// pattern as CurlHttpTransport in dynamodb/.
class CurlGetTransport : public HttpGetTransport {
 public:
  CurlGetTransport();
  ~CurlGetTransport() override;

  HttpGetResponse Get(const std::string& url) override;

 private:
  int timeout_ms_ = 5000;
};

}  // namespace ridgeline

#include "ridgeline/curl_get_transport.h"

#include <curl/curl.h>

namespace ridgeline {

namespace {
std::size_t WriteCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}
}  // namespace

CurlGetTransport::CurlGetTransport() { curl_global_init(CURL_GLOBAL_DEFAULT); }
CurlGetTransport::~CurlGetTransport() { curl_global_cleanup(); }

HttpGetResponse CurlGetTransport::Get(const std::string& url) {
  CURL* curl = curl_easy_init();
  HttpGetResponse response;
  if (!curl) return response;  // status_code stays 0: "no response at all."

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms_));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  const CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status_code = static_cast<int>(status);
    response.body = response_body;
  }
  // res != CURLE_OK leaves status_code at 0 -- WeatherClient treats that
  // identically to any non-200 response: no usable data.

  curl_easy_cleanup(curl);
  return response;
}

}  // namespace ridgeline

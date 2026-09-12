#include "ridgeline/curl_http_transport.h"

#include <curl/curl.h>

namespace ridgeline {

namespace {
std::size_t WriteCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}
}  // namespace

CurlHttpTransport::CurlHttpTransport() { curl_global_init(CURL_GLOBAL_DEFAULT); }
CurlHttpTransport::~CurlHttpTransport() { curl_global_cleanup(); }

HttpResponse CurlHttpTransport::Post(const std::string& url, const std::string& body,
                                    const std::map<std::string, std::string>& headers) {
  CURL* curl = curl_easy_init();
  HttpResponse response;
  if (!curl) return response;  // status_code stays 0: "no response at all."

  struct curl_slist* header_list = nullptr;
  for (const auto& [name, value] : headers) {
    header_list = curl_slist_append(header_list, (name + ": " + value).c_str());
  }

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms_));

  const CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status_code = static_cast<int>(status);
    response.body = response_body;
  }
  // res != CURLE_OK (connection refused, timeout, DNS failure, ...) leaves
  // status_code at 0 -- DeviceShadowStore treats that identically to a
  // non-200 HTTP response: no usable data, not a reason to throw.

  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  return response;
}

}  // namespace ridgeline

#pragma once

#include "ridgeline/device_shadow_store.h"

namespace ridgeline {

// Real network implementation of HttpTransport, using libcurl. Kept out of
// device_shadow_store.h/.cc so those files (and anything that only needs to
// UNIT test DeviceShadowStore's logic) never need libcurl's headers -- only
// code that actually wants to talk to a real DynamoDB endpoint includes
// this file. Same "keep the dependency confined to where it's needed"
// pattern as VideoFileFrameSource hiding OpenCV.
class CurlHttpTransport : public HttpTransport {
 public:
  CurlHttpTransport();
  ~CurlHttpTransport() override;

  HttpResponse Post(const std::string& url, const std::string& body,
                    const std::map<std::string, std::string>& headers) override;

 private:
  int timeout_ms_ = 3000;
};

}  // namespace ridgeline

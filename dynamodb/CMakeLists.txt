add_library(ridgeline_dynamodb STATIC
  src/sigv4_signer.cc
  src/device_shadow_store.cc
  src/curl_http_transport.cc)
target_include_directories(ridgeline_dynamodb PUBLIC include)
target_link_libraries(ridgeline_dynamodb
  PUBLIC ridgeline_core nlohmann_json::nlohmann_json
  PRIVATE OpenSSL::Crypto CURL::libcurl ridgeline_warnings)

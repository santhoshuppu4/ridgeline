add_library(ridgeline_weather STATIC
  src/weather_client.cc
  src/curl_get_transport.cc
  src/alert_engine.cc)
target_include_directories(ridgeline_weather PUBLIC include)
target_link_libraries(ridgeline_weather
  PUBLIC ridgeline_core nlohmann_json::nlohmann_json
  PRIVATE CURL::libcurl ridgeline_warnings)

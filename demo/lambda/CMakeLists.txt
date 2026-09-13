# Demo Lambda module. The core logic (demo_handler_core, dynamo_demo_writer)
# is a plain library with zero Lambda-runtime dependency -- fully unit-
# testable. handler.cc (the actual aws-lambda-cpp entry point) is only
# built when RIDGELINE_BUILD_DEMO_LAMBDA_BINARY is explicitly requested,
# since it needs aws-lambda-cpp available as an external target -- see
# demo/README.md for how the real container build provides that.

add_library(ridgeline_demo_core STATIC
  src/demo_handler_core.cc
  src/dynamo_demo_writer.cc)
target_include_directories(ridgeline_demo_core PUBLIC include)
target_link_libraries(ridgeline_demo_core
  PUBLIC ridgeline_weather ridgeline_dynamodb nlohmann_json::nlohmann_json
  PRIVATE ridgeline_warnings)

if(RIDGELINE_BUILD_DEMO_LAMBDA_BINARY)
  add_executable(ridgeline_demo_handler src/handler.cc)
  target_link_libraries(ridgeline_demo_handler PRIVATE ridgeline_demo_core AWS::aws-lambda-runtime)
endif()

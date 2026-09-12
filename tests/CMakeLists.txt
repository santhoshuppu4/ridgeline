find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(core_tests backoff_test.cc ring_buffer_test.cc kofn_confirmer_test.cc pipeline_test.cc wal_test.cc)
target_link_libraries(core_tests PRIVATE ridgeline_core ridgeline_warnings Threads::Threads GTest::gtest_main)
gtest_discover_tests(core_tests)

if(RIDGELINE_WITH_ONNX)
  # Separate binary: needs a model file and real inference, so it's slower and
  # has external data requirements the core tests deliberately don't.
  add_executable(onnx_tests onnx/onnx_cpu_detector_test.cc onnx/video_pipeline_test.cc)
  target_link_libraries(onnx_tests PRIVATE ridgeline_onnx ridgeline_capture ridgeline_ext_opencv ridgeline_warnings
                                           Threads::Threads GTest::gtest_main)
  target_compile_definitions(onnx_tests PRIVATE
    RIDGELINE_MODEL_PATH="${RIDGELINE_MODEL_PATH}"
    RIDGELINE_TEST_IMAGE="${RIDGELINE_TEST_IMAGE}")
  gtest_discover_tests(onnx_tests DISCOVERY_TIMEOUT 30)
endif()

if(RIDGELINE_WITH_KAFKA)
  add_executable(kafka_tests kafka/kafka_producer_test.cc)
  target_link_libraries(kafka_tests PRIVATE ridgeline_kafka ridgeline_ext_rdkafka ridgeline_warnings
                                            Threads::Threads GTest::gtest_main)
  gtest_discover_tests(kafka_tests DISCOVERY_TIMEOUT 30)
endif()

if(RIDGELINE_WITH_REDIS)
  add_executable(redis_tests redis/redis_hot_state_store_test.cc)
  target_link_libraries(redis_tests PRIVATE ridgeline_redis ridgeline_ext_hiredis ridgeline_warnings
                                            Threads::Threads GTest::gtest_main)
  gtest_discover_tests(redis_tests DISCOVERY_TIMEOUT 30)
endif()

if(RIDGELINE_WITH_DYNAMODB)
  add_executable(dynamodb_tests dynamodb/sigv4_signer_test.cc dynamodb/device_shadow_store_test.cc)
  target_link_libraries(dynamodb_tests PRIVATE ridgeline_dynamodb nlohmann_json::nlohmann_json ridgeline_warnings
                                               Threads::Threads GTest::gtest_main)
  gtest_discover_tests(dynamodb_tests DISCOVERY_TIMEOUT 30)
endif()

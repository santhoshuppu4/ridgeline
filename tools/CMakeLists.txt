if(RIDGELINE_WITH_ONNX)
  add_executable(ridgeline_edge ridgeline_edge.cc)
  target_link_libraries(ridgeline_edge PRIVATE ridgeline_onnx ridgeline_capture ridgeline_warnings Threads::Threads)
  target_compile_definitions(ridgeline_edge PRIVATE RIDGELINE_DEFAULT_MODEL="${RIDGELINE_MODEL_PATH}")

  add_executable(ridgeline_infer_bench infer_bench.cc)
  target_link_libraries(ridgeline_infer_bench PRIVATE ridgeline_onnx ridgeline_ext_opencv ridgeline_warnings)
  target_compile_definitions(ridgeline_infer_bench PRIVATE
    RIDGELINE_DEFAULT_MODEL="${RIDGELINE_MODEL_PATH}"
    RIDGELINE_DEFAULT_IMAGE="${RIDGELINE_TEST_IMAGE}")

  add_executable(ridgeline_make_sample_video make_sample_video.cc)
  target_link_libraries(ridgeline_make_sample_video PRIVATE ridgeline_ext_opencv ridgeline_warnings)
  target_compile_definitions(ridgeline_make_sample_video PRIVATE RIDGELINE_DEFAULT_IMAGE="${RIDGELINE_TEST_IMAGE}")
endif()

if(RIDGELINE_WITH_KAFKA)
  add_executable(ridgeline_mock_kafka_broker mock_kafka_broker.cc)
  target_link_libraries(ridgeline_mock_kafka_broker PRIVATE ridgeline_ext_rdkafka ridgeline_warnings Threads::Threads)
endif()

if(RIDGELINE_BUILD_NET)
  add_executable(ridgeline_device_simulator fleet/device_simulator.cc)
  target_include_directories(ridgeline_device_simulator PRIVATE .)
  target_link_libraries(ridgeline_device_simulator PRIVATE ridgeline_core ridgeline_proto ridgeline_warnings Threads::Threads)
endif()

if(RIDGELINE_BUILD_NET)
  add_executable(ridgeline_gap_protocol_test fleet/gap_protocol_test.cc)
  target_include_directories(ridgeline_gap_protocol_test PRIVATE .)
  target_link_libraries(ridgeline_gap_protocol_test PRIVATE ridgeline_core ridgeline_proto ridgeline_warnings Threads::Threads)
endif()

add_executable(ridgeline_ota_tool ota_tool.cc)
target_link_libraries(ridgeline_ota_tool PRIVATE ridgeline_ota ridgeline_core ridgeline_warnings)

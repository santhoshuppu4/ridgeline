find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(core_tests backoff_test.cc ring_buffer_test.cc kofn_confirmer_test.cc pipeline_test.cc)
target_link_libraries(core_tests PRIVATE ridgeline_core ridgeline_warnings Threads::Threads GTest::gtest_main)
gtest_discover_tests(core_tests)

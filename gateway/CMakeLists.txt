add_executable(ridgeline_gateway src/main.cc)
target_link_libraries(ridgeline_gateway PRIVATE ridgeline_core ridgeline_proto ridgeline_warnings Threads::Threads)

if(RIDGELINE_WITH_KAFKA)
  target_link_libraries(ridgeline_gateway PRIVATE ridgeline_kafka)
  target_compile_definitions(ridgeline_gateway PRIVATE RIDGELINE_HAVE_KAFKA=1)
endif()

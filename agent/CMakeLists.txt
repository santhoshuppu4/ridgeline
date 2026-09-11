add_executable(ridgeline_agent src/main.cc)
target_compile_definitions(ridgeline_agent PRIVATE RIDGELINE_VERSION="${PROJECT_VERSION}")
target_link_libraries(ridgeline_agent PRIVATE ridgeline_core ridgeline_proto ridgeline_warnings Threads::Threads)

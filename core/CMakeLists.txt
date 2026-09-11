# ring_buffer.h and frame.h are header-only templates, so they need no .cc
# file here — they just ride along on the public include path.
add_library(ridgeline_core STATIC
  src/backoff.cc
  src/wal.cc)
target_include_directories(ridgeline_core PUBLIC include)
target_link_libraries(ridgeline_core PRIVATE ridgeline_warnings)

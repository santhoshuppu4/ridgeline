# Optional Phase 1b/1d dependencies, each gated by its own RIDGELINE_WITH_*
# flag. This file is included whenever ANY of ONNX/KAFKA/REDIS/DYNAMODB is
# on, so every block below MUST check its own flag before doing anything --
# a block that runs unconditionally will fail a CI job that enabled a
# DIFFERENT flag and never installed that block's system dependency.
#
# This was a real bug, not a hypothetical one: the ONNX Runtime and OpenCV
# blocks were originally written back when this file was only ever included
# under RIDGELINE_WITH_ONNX, so they never needed their own guard. Once
# RIDGELINE_WITH_KAFKA/REDIS/DYNAMODB were added and the top-level
# CMakeLists.txt's include() condition became an OR of all four flags, those
# two blocks started running for Kafka-only, Redis-only, and DynamoDB-only
# configurations too -- and failed, because THOSE CI jobs correctly don't
# install ONNX Runtime or OpenCV at all. Symmetrically, the kafka/hiredis/
# curl blocks (added later, appended without a guard) then broke the
# onnx-inference job the same way, in reverse. Caught by CI running each
# flag combination in its own job with a genuinely clean checkout -- not
# caught locally, because a developer machine that has already built every
# phase has stale ONNX Runtime/model files sitting under third_party/ that
# happen to satisfy the unconditional find_path/find_library calls even
# without the matching flag set. "Works on my machine, fails in CI" here
# specifically meant "my machine has leftover state from earlier phases that
# masks a missing guard."

if(RIDGELINE_WITH_ONNX)
  # ONNX RUNTIME GOTCHA: the official onnxruntime-linux-x64-<ver>.tgz ships a
  # lib/cmake/onnxruntime/onnxruntimeConfig.cmake that points at lib64/ and
  # include/onnxruntime/ -- directories that do NOT exist in the tarball (it
  # has lib/ and include/). find_package(onnxruntime) therefore "succeeds"
  # and then fails at build time. We define the imported target ourselves
  # from the directory layout that actually ships. See context/adr/0005.
  set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime-linux-x64-1.23.2"
      CACHE PATH "Extracted ONNX Runtime prebuilt directory (run scripts/fetch-phase1b-assets.sh)")

  find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h PATHS "${ONNXRUNTIME_ROOT}/include" NO_DEFAULT_PATH)
  find_library(ONNXRUNTIME_LIBRARY onnxruntime PATHS "${ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH)
  if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR
      "RIDGELINE_WITH_ONNX=ON but ONNX Runtime was not found under ONNXRUNTIME_ROOT=${ONNXRUNTIME_ROOT}.\n"
      "Run ./scripts/fetch-phase1b-assets.sh first, or pass -DONNXRUNTIME_ROOT=/path/to/onnxruntime-linux-x64-<ver>.")
  endif()
  add_library(ridgeline_ext_onnxruntime SHARED IMPORTED GLOBAL)
  set_target_properties(ridgeline_ext_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}")
  message(STATUS "ONNX Runtime: ${ONNXRUNTIME_LIBRARY}")

  # OpenCV: prefer its CMake package (from libopencv-dev); fall back to
  # locating the individual component libraries, which is all some minimal
  # installs ship.
  find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs videoio)
  add_library(ridgeline_ext_opencv INTERFACE)
  if(OpenCV_FOUND)
    target_link_libraries(ridgeline_ext_opencv INTERFACE ${OpenCV_LIBS})
    target_include_directories(ridgeline_ext_opencv SYSTEM INTERFACE ${OpenCV_INCLUDE_DIRS})
    message(STATUS "OpenCV ${OpenCV_VERSION} via CMake package")
  else()
    find_path(RIDGELINE_OPENCV_INCLUDE opencv2/core.hpp PATH_SUFFIXES opencv4)
    foreach(comp core imgproc imgcodecs videoio)
      find_library(RIDGELINE_OPENCV_${comp} opencv_${comp})
      if(NOT RIDGELINE_OPENCV_${comp})
        message(FATAL_ERROR "OpenCV component ${comp} not found. Install: sudo apt-get install -y libopencv-dev")
      endif()
      target_link_libraries(ridgeline_ext_opencv INTERFACE ${RIDGELINE_OPENCV_${comp}})
    endforeach()
    if(NOT RIDGELINE_OPENCV_INCLUDE)
      message(FATAL_ERROR "OpenCV headers not found. Install: sudo apt-get install -y libopencv-dev")
    endif()
    target_include_directories(ridgeline_ext_opencv SYSTEM INTERFACE ${RIDGELINE_OPENCV_INCLUDE})
    message(STATUS "OpenCV via component libraries: ${RIDGELINE_OPENCV_INCLUDE}")
  endif()

  set(RIDGELINE_MODEL_PATH "${CMAKE_SOURCE_DIR}/third_party/models/yolox_nano.onnx"
      CACHE FILEPATH "YOLOX ONNX model used by tests and tools")
  set(RIDGELINE_TEST_IMAGE "${CMAKE_SOURCE_DIR}/third_party/testdata/dog.jpg"
      CACHE FILEPATH "Reference image with known objects, used by detector tests")
endif()

if(RIDGELINE_WITH_KAFKA)
  # librdkafka: apt package ships proper CMake-free headers/libs, found via
  # find_path/find_library rather than find_package (it has no CMake config).
  find_path(RIDGELINE_RDKAFKA_INCLUDE librdkafka/rdkafkacpp.h)
  find_library(RIDGELINE_RDKAFKA_LIB rdkafka++)
  find_library(RIDGELINE_RDKAFKA_C_LIB rdkafka)
  if(NOT RIDGELINE_RDKAFKA_INCLUDE OR NOT RIDGELINE_RDKAFKA_LIB)
    message(FATAL_ERROR "librdkafka not found. Install: sudo apt-get install -y librdkafka-dev")
  endif()
  add_library(ridgeline_ext_rdkafka INTERFACE)
  target_include_directories(ridgeline_ext_rdkafka SYSTEM INTERFACE ${RIDGELINE_RDKAFKA_INCLUDE})
  target_link_libraries(ridgeline_ext_rdkafka INTERFACE ${RIDGELINE_RDKAFKA_LIB} ${RIDGELINE_RDKAFKA_C_LIB})
  message(STATUS "librdkafka: ${RIDGELINE_RDKAFKA_LIB}")
endif()

if(RIDGELINE_WITH_REDIS)
  # hiredis: minimal C client for Redis, no CMake config -- found by path/lib.
  find_path(RIDGELINE_HIREDIS_INCLUDE hiredis/hiredis.h)
  find_library(RIDGELINE_HIREDIS_LIB hiredis)
  if(NOT RIDGELINE_HIREDIS_INCLUDE OR NOT RIDGELINE_HIREDIS_LIB)
    message(FATAL_ERROR "hiredis not found. Install: sudo apt-get install -y libhiredis-dev")
  endif()
  add_library(ridgeline_ext_hiredis INTERFACE)
  target_include_directories(ridgeline_ext_hiredis SYSTEM INTERFACE ${RIDGELINE_HIREDIS_INCLUDE})
  target_link_libraries(ridgeline_ext_hiredis INTERFACE ${RIDGELINE_HIREDIS_LIB})
  message(STATUS "hiredis: ${RIDGELINE_HIREDIS_LIB}")
endif()

if(RIDGELINE_WITH_DYNAMODB)
  # libcurl + nlohmann-json + OpenSSL: HTTP transport, JSON, and SigV4 HMAC
  # for the DynamoDB shadow client.
  find_package(CURL REQUIRED)
  find_package(nlohmann_json REQUIRED)
  find_package(OpenSSL REQUIRED)
endif()

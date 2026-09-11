cmake_minimum_required(VERSION 3.22)
project(ridgeline VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RIDGELINE_BUILD_NET "Build the gRPC agent and gateway (needs gRPC/protobuf)" ON)
option(RIDGELINE_WITH_ONNX "Build ONNX Runtime inference + OpenCV video capture (Phase 1b-ii)" OFF)
set(RIDGELINE_SANITIZE "" CACHE STRING "Sanitizer build: '', 'address' (ASan+UBSan), or 'thread' (TSan)")

include(cmake/Sanitizers.cmake)

add_library(ridgeline_warnings INTERFACE)
target_compile_options(ridgeline_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

find_package(Threads REQUIRED)
enable_testing()

if(RIDGELINE_WITH_ONNX AND RIDGELINE_SANITIZE STREQUAL "thread")
  message(FATAL_ERROR "RIDGELINE_WITH_ONNX is not supported with TSan: prebuilt ONNX Runtime is uninstrumented "
                      "and its thread pool produces false-positive race reports.")
endif()

# IMPORTANT ORDERING: cmake/Dependencies.cmake defines RIDGELINE_MODEL_PATH
# (among other cache vars), which agent/CMakeLists.txt consumes to set the
# agent's default --model path at compile time via RIDGELINE_DEFAULT_MODEL.
# It must be included BEFORE add_subdirectory(agent), or that macro silently
# ends up as an empty string -- caught the hard way once: the agent linked
# and ran fine, but crashed at startup in --video mode with "Load model from
#  failed: File doesn't exist" (note the blank path). CMake variable
# ordering bugs like this produce no error at configure or build time, only
# at runtime, which is exactly why this dependency is called out explicitly
# here rather than left to be rediscovered.
if(RIDGELINE_WITH_ONNX)
  include(cmake/Dependencies.cmake)
endif()

add_subdirectory(core)
if(RIDGELINE_BUILD_NET)
  add_subdirectory(proto)
  add_subdirectory(agent)
  add_subdirectory(gateway)
endif()
if(RIDGELINE_WITH_ONNX)
  add_subdirectory(inference)
  add_subdirectory(capture)
  add_subdirectory(tools)
endif()
add_subdirectory(tests)
add_subdirectory(bench)

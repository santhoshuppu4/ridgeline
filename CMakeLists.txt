cmake_minimum_required(VERSION 3.22)
project(ridgeline VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RIDGELINE_BUILD_NET "Build the gRPC agent and gateway (needs gRPC/protobuf)" ON)
set(RIDGELINE_SANITIZE "" CACHE STRING "Sanitizer build: '', 'address' (ASan+UBSan), or 'thread' (TSan)")

include(cmake/Sanitizers.cmake)

add_library(ridgeline_warnings INTERFACE)
target_compile_options(ridgeline_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

find_package(Threads REQUIRED)
enable_testing()

add_subdirectory(core)
if(RIDGELINE_BUILD_NET)
  add_subdirectory(proto)
  add_subdirectory(agent)
  add_subdirectory(gateway)
endif()
add_subdirectory(tests)

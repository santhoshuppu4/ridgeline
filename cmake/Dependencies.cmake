# Optional Phase 1b-ii dependencies: ONNX Runtime (inference) and OpenCV (video).
#
# ONNX RUNTIME GOTCHA: the official onnxruntime-linux-x64-<ver>.tgz ships a
# lib/cmake/onnxruntime/onnxruntimeConfig.cmake that points at lib64/ and
# include/onnxruntime/ — directories that do NOT exist in the tarball (it has
# lib/ and include/). find_package(onnxruntime) therefore "succeeds" and then
# fails at build time. We define the imported target ourselves from the
# directory layout that actually ships. See context/adr/0005.

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

# OpenCV: prefer its CMake package (from libopencv-dev); fall back to locating
# the individual component libraries, which is all some minimal installs ship.
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

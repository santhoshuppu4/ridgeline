find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

set(PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(PROTO_FILES ${PROTO_ROOT}/ridgeline/v1/ingest.proto)

set(GEN_SRCS
  ${GEN_DIR}/ridgeline/v1/ingest.pb.cc
  ${GEN_DIR}/ridgeline/v1/ingest.pb.h
  ${GEN_DIR}/ridgeline/v1/ingest.grpc.pb.cc
  ${GEN_DIR}/ridgeline/v1/ingest.grpc.pb.h)

add_custom_command(
  OUTPUT ${GEN_SRCS}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_DIR}
  COMMAND protobuf::protoc
          --proto_path=${PROTO_ROOT}
          --cpp_out=${GEN_DIR}
          --grpc_out=${GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          ${PROTO_FILES}
  DEPENDS ${PROTO_FILES}
  VERBATIM)

add_library(ridgeline_proto STATIC ${GEN_SRCS})
target_include_directories(ridgeline_proto SYSTEM PUBLIC ${GEN_DIR})
target_link_libraries(ridgeline_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

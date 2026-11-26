set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated}")
add_custom_command(
    OUTPUT "${generated}/smoke.pb.cc" "${generated}/smoke.pb.h"
           "${generated}/smoke.grpc.pb.cc" "${generated}/smoke.grpc.pb.h"
    COMMAND $<TARGET_FILE:protobuf::protoc>
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}"
        "--cpp_out=${generated}" "--grpc_out=${generated}"
        "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
        "${CMAKE_CURRENT_SOURCE_DIR}/smoke.proto"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/smoke.proto"
            protobuf::protoc gRPC::grpc_cpp_plugin
    VERBATIM)

add_executable(test_grpc_smoke smoke.cpp
    "${generated}/smoke.pb.cc" "${generated}/smoke.grpc.pb.cc")
target_include_directories(test_grpc_smoke PRIVATE "${generated}")
target_link_libraries(test_grpc_smoke PRIVATE packetia_grpc)
add_test(NAME GrpcSmokeTest COMMAND test_grpc_smoke)
set_tests_properties(GrpcSmokeTest PROPERTIES TIMEOUT 15)

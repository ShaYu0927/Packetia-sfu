get_filename_component(agent_source_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(packetia_agent_worker_api INTERFACE)
target_compile_features(packetia_agent_worker_api INTERFACE cxx_std_17)
target_include_directories(packetia_agent_worker_api INTERFACE "${agent_source_dir}")

add_library(packetia_agent_worker STATIC "${agent_source_dir}/LocalAgentWorker.cpp")
target_link_libraries(packetia_agent_worker PUBLIC packetia_agent_worker_api)

if(TARGET packetia_grpc)
    set(agent_proto "${CMAKE_CURRENT_SOURCE_DIR}/agent_worker.proto")
    set(agent_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_custom_command(
        OUTPUT "${agent_generated}/agent_worker.pb.cc" "${agent_generated}/agent_worker.pb.h"
               "${agent_generated}/agent_worker.grpc.pb.cc" "${agent_generated}/agent_worker.grpc.pb.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agent_generated}"
        COMMAND $<TARGET_FILE:protobuf::protoc>
            "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}"
            "--cpp_out=${agent_generated}" "--grpc_out=${agent_generated}"
            "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>" "${agent_proto}"
        DEPENDS "${agent_proto}" protobuf::protoc gRPC::grpc_cpp_plugin
        VERBATIM)
    add_library(packetia_agent_rpc STATIC "${agent_source_dir}/GrpcAgentWorkerClient.cpp"
        "${agent_generated}/agent_worker.pb.cc" "${agent_generated}/agent_worker.grpc.pb.cc")
    target_include_directories(packetia_agent_rpc PUBLIC "${agent_generated}")
    target_link_libraries(packetia_agent_rpc PUBLIC packetia_agent_worker_api packetia_grpc)
endif()

if(TARGET packetia_libwebsockets)
    find_package(Threads REQUIRED)
    add_library(packetia_agent_room_websocket STATIC "${agent_source_dir}/AgentRoomClient.cpp")
    target_compile_features(packetia_agent_room_websocket PUBLIC cxx_std_17)
    target_include_directories(packetia_agent_room_websocket
        PUBLIC "${agent_source_dir}"
        PRIVATE "${agent_source_dir}/../..")
    target_link_libraries(packetia_agent_room_websocket PRIVATE packetia_libwebsockets Threads::Threads)
endif()

cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaMemoryTests LANGUAGES CXX)
    enable_testing()
endif()

find_package(Threads REQUIRED)
if(NOT TARGET packetia_memory)
    add_subdirectory(.. memory-library)
endif()
get_filename_component(PACKETIA_MEMORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_executable(test_memory_model
    test_memory_model.cpp
    "${PACKETIA_MEMORY_ROOT}/Rtsp/Rtp.cpp"
    "${PACKETIA_MEMORY_ROOT}/Common/Depacketizer/H264Depacketizer.cpp"
    "${PACKETIA_MEMORY_ROOT}/Common/Depacketizer/H264RtpPayloadParser.cpp"
    "${PACKETIA_MEMORY_ROOT}/Common/Depacketizer/H26xPacketBuffer.cpp"
    "${PACKETIA_MEMORY_ROOT}/Common/Depacketizer/H264ParameterSetTracker.cpp"
    "${PACKETIA_MEMORY_ROOT}/utils/ShardedWorkerPool.cpp"
    "${PACKETIA_MEMORY_ROOT}/utils/EndpointBase.cpp"
    "${PACKETIA_MEMORY_ROOT}/media/transport/MediaEndpointIngress.cpp"
    "${PACKETIA_MEMORY_ROOT}/media/core/EncodedFrameRouter.cpp"
    "${PACKETIA_MEMORY_ROOT}/Common/Depacketizer/AudioDepacketizer.cpp"
    "${PACKETIA_MEMORY_ROOT}/Common/Depacketizer/AudioRtpDepacketizerFactory.cpp")
target_compile_features(test_memory_model PRIVATE cxx_std_17)
target_include_directories(test_memory_model PRIVATE
    "${PACKETIA_MEMORY_ROOT}"
    "${PACKETIA_MEMORY_ROOT}/utils"
    "${PACKETIA_MEMORY_ROOT}/Rtsp"
    "${PACKETIA_MEMORY_ROOT}/Rtsp/Rtp"
    "${PACKETIA_MEMORY_ROOT}/protocol/Rtcp"
    "${PACKETIA_MEMORY_ROOT}/media/transport"
    "${PACKETIA_MEMORY_ROOT}/log"
    "${PACKETIA_MEMORY_ROOT}/Common/Depacketizer"
    "${PACKETIA_MEMORY_ROOT}/media/MediaFrame")
target_link_libraries(test_memory_model PRIVATE Threads::Threads packetia_memory)

add_test(NAME memory_model COMMAND test_memory_model)
set_tests_properties(memory_model PROPERTIES TIMEOUT 30)

add_executable(test_memory_pool test_memory_pool.cpp)
target_include_directories(test_memory_pool PRIVATE "${PACKETIA_MEMORY_ROOT}")
target_link_libraries(test_memory_pool PRIVATE packetia_memory)
add_test(NAME memory_pool COMMAND test_memory_pool)
set_tests_properties(memory_pool PROPERTIES TIMEOUT 30)

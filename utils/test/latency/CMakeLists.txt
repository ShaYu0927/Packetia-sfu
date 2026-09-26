cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaLatencyTests LANGUAGES CXX)
    enable_testing()
endif()
find_package(Threads REQUIRED)
get_filename_component(PACKETIA_LATENCY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET packetia_memory)
    add_subdirectory("${PACKETIA_LATENCY_ROOT}/Common/memory" memory-library)
endif()

add_library(latency_test_support STATIC "${PACKETIA_LATENCY_ROOT}/utils/MediaLatency.cpp")
target_compile_features(latency_test_support PUBLIC cxx_std_17)
target_include_directories(latency_test_support PUBLIC
    "${PACKETIA_LATENCY_ROOT}" "${PACKETIA_LATENCY_ROOT}/utils"
    "${PACKETIA_LATENCY_ROOT}/log" "${PACKETIA_LATENCY_ROOT}/media/transport")
target_link_libraries(latency_test_support PUBLIC Threads::Threads)

add_executable(test_media_latency test_media_latency.cpp)
target_link_libraries(test_media_latency PRIVATE latency_test_support)
add_test(NAME media_latency COMMAND test_media_latency)
add_test(NAME media_latency_disabled COMMAND test_media_latency disabled)
set_tests_properties(media_latency_disabled PROPERTIES ENVIRONMENT "PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY=0")
add_test(NAME media_latency_sampling COMMAND test_media_latency sampling)
set_tests_properties(media_latency_sampling PROPERTIES ENVIRONMENT "PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY=4")

add_executable(test_media_latency_worker test_media_latency_worker.cpp
    "${PACKETIA_LATENCY_ROOT}/utils/ShardedWorkerPool.cpp"
    "${PACKETIA_LATENCY_ROOT}/media/transport/MediaEndpointIngress.cpp")
target_link_libraries(test_media_latency_worker PRIVATE latency_test_support packetia_memory)
add_test(NAME media_latency_worker COMMAND test_media_latency_worker)
set_tests_properties(media_latency media_latency_worker PROPERTIES
    ENVIRONMENT "PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY=1;PACKETIA_MEDIA_LATENCY_INTERVAL_MS=3600000"
    TIMEOUT 20)

# Keep these tests runnable without FFmpeg, GTest or the network backend.
set(PACKETIA_LATENCY_NETWORK_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    set(PACKETIA_LATENCY_NETWORK_DEFAULT ON)
endif()
option(PACKETIA_LATENCY_NETWORK_TESTS "Test TCP completion against scripted send results"
    ${PACKETIA_LATENCY_NETWORK_DEFAULT})
if(PACKETIA_LATENCY_NETWORK_TESTS)
    add_executable(test_media_latency_buffer test_media_latency_buffer.cpp
        "${PACKETIA_LATENCY_ROOT}/network/BufferWrite.cpp"
        "${PACKETIA_LATENCY_ROOT}/network/SocketUtil.cpp")
    target_include_directories(test_media_latency_buffer PRIVATE "${PACKETIA_LATENCY_ROOT}/network")
    target_link_libraries(test_media_latency_buffer PRIVATE latency_test_support packetia_memory)
    add_test(NAME media_latency_buffer COMMAND test_media_latency_buffer)
    set_tests_properties(media_latency_buffer PROPERTIES TIMEOUT 20)
endif()

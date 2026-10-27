cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaServerTests LANGUAGES CXX)
    enable_testing()
    find_package(Threads REQUIRED)
    get_filename_component(PACKETIA_SERVER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    include("${PACKETIA_SERVER_ROOT}/utils/threading/ThreadSupport.cmake")
    add_subdirectory("${PACKETIA_SERVER_ROOT}/config" "${CMAKE_CURRENT_BINARY_DIR}/config")
    add_library(server_test_support STATIC
        "${PACKETIA_SERVER_ROOT}/server/ServerLauncher.cpp"
        "${PACKETIA_SERVER_ROOT}/server/ServerConfig.cpp"
        "${PACKETIA_SERVER_ROOT}/server/StartupConfig.cpp")
    target_include_directories(server_test_support PUBLIC
        "${PACKETIA_SERVER_ROOT}"
        "${PACKETIA_SERVER_ROOT}/server"
        "${PACKETIA_SERVER_ROOT}/service/core"
        "${PACKETIA_SERVER_ROOT}/log")
    target_compile_features(server_test_support PUBLIC cxx_std_17)
    target_link_libraries(server_test_support PUBLIC packetia_threads packetia_config)
    set(PACKETIA_SERVER_TEST_LIBRARY server_test_support)
else()
    set(PACKETIA_SERVER_TEST_LIBRARY server)
endif()

add_executable(test_server_launcher test_server_launcher.cpp)
target_link_libraries(test_server_launcher PRIVATE ${PACKETIA_SERVER_TEST_LIBRARY})
add_test(NAME server_launcher COMMAND test_server_launcher)
set_tests_properties(server_launcher PROPERTIES TIMEOUT 20)

add_executable(test_server_config test_server_config.cpp)
target_link_libraries(test_server_config PRIVATE ${PACKETIA_SERVER_TEST_LIBRARY})
add_test(NAME server_config COMMAND test_server_config)

add_executable(test_startup_config test_startup_config.cpp)
target_link_libraries(test_startup_config PRIVATE ${PACKETIA_SERVER_TEST_LIBRARY})
add_test(NAME startup_config COMMAND test_startup_config "${CMAKE_CURRENT_LIST_DIR}/../../config/packetia.json")

# Exercise real ingress/worker/session control with a fake persistence layer.
get_filename_component(PACKETIA_SERVER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET packetia_monitoring)
    add_subdirectory("${PACKETIA_SERVER_ROOT}/monitoring" monitoring-library)
endif()
add_executable(test_stream_services
    test_stream_services.cpp recording_instance_stub.cpp
    ${PACKETIA_SERVER_ROOT}/service/RecordService/RecordingService.cpp
    ${PACKETIA_SERVER_ROOT}/service/RecordService/RecordingDispatcher.cpp
    ${PACKETIA_SERVER_ROOT}/service/RecordService/RecordingSession.cpp
    ${PACKETIA_SERVER_ROOT}/service/AIService/AIFrameIngress.cpp
    ${PACKETIA_SERVER_ROOT}/service/AIService/AIService.cpp
    ${PACKETIA_SERVER_ROOT}/service/AIService/AIOrchestrator.cpp
    ${PACKETIA_SERVER_ROOT}/service/AIService/UnavailableModelProvider.cpp
    ${PACKETIA_SERVER_ROOT}/media/core/EncodedFrameRouter.cpp
    ${PACKETIA_SERVER_ROOT}/utils/WorkerRegistry.cpp
    ${PACKETIA_SERVER_ROOT}/utils/ShardedWorkerPool.cpp
    ${PACKETIA_SERVER_ROOT}/utils/MediaLatency.cpp
)
target_include_directories(test_stream_services PRIVATE
    ${PACKETIA_SERVER_ROOT} ${PACKETIA_SERVER_ROOT}/service ${PACKETIA_SERVER_ROOT}/media
    ${PACKETIA_SERVER_ROOT}/media/core ${PACKETIA_SERVER_ROOT}/utils ${PACKETIA_SERVER_ROOT}/log)
target_include_directories(test_stream_services PRIVATE
    ${PACKETIA_SERVER_ROOT}/service/core
    ${PACKETIA_SERVER_ROOT}/service/AIService
    ${PACKETIA_SERVER_ROOT}/media/MediaFrame
    ${PACKETIA_SERVER_ROOT}/media/types
    ${PACKETIA_SERVER_ROOT}/Common/memory)
if(NOT TARGET packetia_recording_index)
    add_subdirectory("${PACKETIA_SERVER_ROOT}/service/RecordService/Index" recording-index)
endif()
target_link_libraries(test_stream_services PRIVATE packetia_config packetia_recording_index packetia_threads packetia_monitoring)
add_test(NAME stream_services COMMAND test_stream_services)
set_tests_properties(stream_services PROPERTIES TIMEOUT 30)

if(TARGET media AND PACKETIA_WITH_LIBWEBSOCKETS AND PACKETIA_SRTP_AVAILABLE)
    add_executable(test_webrtc_service test_webrtc_service.cpp)
    target_link_libraries(test_webrtc_service PRIVATE
        "$<$<PLATFORM_ID:Linux>:-Wl,--start-group>"
        server packetia_signaling ROOM_LIB media network utils WebRtc LOGGER packetia_auth
        "$<$<PLATFORM_ID:Linux>:-Wl,--end-group>")
    add_test(NAME webrtc_service COMMAND test_webrtc_service)
    set_tests_properties(webrtc_service PROPERTIES TIMEOUT 60)
endif()

if(TARGET packetia_signaling)
    add_executable(test_signal_channel test_signal_channel.cpp)
    target_link_libraries(test_signal_channel PRIVATE packetia_signaling GTest::gtest_main)
    add_test(NAME signal_channel COMMAND test_signal_channel)
    set_tests_properties(signal_channel PROPERTIES TIMEOUT 10)
endif()

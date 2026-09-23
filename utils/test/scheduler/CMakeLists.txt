cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaKqueueTests LANGUAGES CXX)
    enable_testing()
endif()
if(NOT APPLE)
    message(FATAL_ERROR "Kqueue scheduler tests require macOS")
endif()

find_package(Threads REQUIRED)
get_filename_component(PACKETIA_SCHEDULER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(test_kqueue_scheduler test_kqueue_scheduler.cpp)
target_compile_features(test_kqueue_scheduler PRIVATE cxx_std_17)
target_include_directories(test_kqueue_scheduler PRIVATE
    "${PACKETIA_SCHEDULER_ROOT}/utils"
    "${PACKETIA_SCHEDULER_ROOT}/network"
    "${PACKETIA_SCHEDULER_ROOT}/log")
if(TARGET utils)
    target_link_libraries(test_kqueue_scheduler PRIVATE utils)
else()
    target_sources(test_kqueue_scheduler PRIVATE
        "${PACKETIA_SCHEDULER_ROOT}/utils/KqueueTaskScheduler.cpp"
        "${PACKETIA_SCHEDULER_ROOT}/utils/EventLoop.cpp"
        "${PACKETIA_SCHEDULER_ROOT}/utils/TaskScheduler.cpp"
        "${PACKETIA_SCHEDULER_ROOT}/utils/Pip.cpp"
        "${PACKETIA_SCHEDULER_ROOT}/utils/TimeUtil.cpp"
        "${PACKETIA_SCHEDULER_ROOT}/utils/MediaLatency.cpp")
endif()
target_link_libraries(test_kqueue_scheduler PRIVATE Threads::Threads)
add_test(NAME KqueueSchedulerTest COMMAND test_kqueue_scheduler)
set_tests_properties(KqueueSchedulerTest PROPERTIES
    ENVIRONMENT "PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY=0"
    TIMEOUT 20)

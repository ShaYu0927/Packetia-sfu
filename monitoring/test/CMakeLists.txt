cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaMonitoringTests LANGUAGES CXX)
    enable_testing()
endif()
find_package(Threads REQUIRED)
get_filename_component(PACKETIA_MONITORING_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET packetia_monitoring)
    add_subdirectory("${PACKETIA_MONITORING_ROOT}/monitoring" monitoring-library)
endif()
include("${PACKETIA_MONITORING_ROOT}/utils/threading/ThreadSupport.cmake")
if(NOT TARGET packetia_monitor_service)
    add_subdirectory("${PACKETIA_MONITORING_ROOT}/service/MonitorService" monitor-service)
endif()
add_executable(test_monitor_service test_monitor_service.cpp)
target_link_libraries(test_monitor_service PRIVATE packetia_monitor_service)
add_test(NAME MonitorServiceTest COMMAND test_monitor_service)
set_tests_properties(MonitorServiceTest PROPERTIES TIMEOUT 20)

add_executable(test_runtime_metrics test_runtime_metrics.cpp)
target_link_libraries(test_runtime_metrics PRIVATE packetia_monitoring)
add_test(NAME RuntimeMetricsTest COMMAND test_runtime_metrics)
set_tests_properties(RuntimeMetricsTest PROPERTIES TIMEOUT 20)

add_executable(test_metrics_routes test_metrics_routes.cpp
    "${PACKETIA_MONITORING_ROOT}/server/api/MetricsController.cpp"
    "${PACKETIA_MONITORING_ROOT}/server/api/MetricsJsonEncoder.cpp"
    "${PACKETIA_MONITORING_ROOT}/server/api/ApiRouter.cpp"
    "${PACKETIA_MONITORING_ROOT}/server/api/ApiResponse.cpp")
target_include_directories(test_metrics_routes PRIVATE
    "${PACKETIA_MONITORING_ROOT}/server/api" "${PACKETIA_MONITORING_ROOT}/protocol/http"
    "${PACKETIA_MONITORING_ROOT}/utils" "${PACKETIA_MONITORING_ROOT}/third")
target_link_libraries(test_metrics_routes PRIVATE packetia_monitor_service)
add_test(NAME MetricsRoutesTest COMMAND test_metrics_routes)
set_tests_properties(MetricsRoutesTest PROPERTIES TIMEOUT 20)

add_executable(test_worker_metrics test_worker_metrics.cpp)
if(TARGET utils)
    target_link_libraries(test_worker_metrics PRIVATE utils)
else()
    include("${PACKETIA_MONITORING_ROOT}/utils/threading/ThreadSupport.cmake")
    add_subdirectory("${PACKETIA_MONITORING_ROOT}/Common/memory" memory-library)
    target_sources(test_worker_metrics PRIVATE
        "${PACKETIA_MONITORING_ROOT}/utils/ShardedWorkerPool.cpp"
        "${PACKETIA_MONITORING_ROOT}/utils/MediaLatency.cpp")
    target_include_directories(test_worker_metrics PRIVATE "${PACKETIA_MONITORING_ROOT}/log")
    target_link_libraries(test_worker_metrics PRIVATE packetia_monitoring packetia_threads packetia_memory)
endif()
target_include_directories(test_worker_metrics PRIVATE "${PACKETIA_MONITORING_ROOT}/utils")
add_test(NAME WorkerMetricsTest COMMAND test_worker_metrics)
set_tests_properties(WorkerMetricsTest PROPERTIES TIMEOUT 20 ENVIRONMENT "PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY=0")

add_executable(test_monitor_api_server test_monitor_api_server.cpp)
if(TARGET packetia_monitoring_api)
    target_link_libraries(test_monitor_api_server PRIVATE packetia_monitoring_api)
else()
    target_sources(test_monitor_api_server PRIVATE
        "${PACKETIA_MONITORING_ROOT}/server/api/ApiServer.cpp"
        "${PACKETIA_MONITORING_ROOT}/server/api/ApiRouter.cpp"
        "${PACKETIA_MONITORING_ROOT}/server/api/ApiResponse.cpp"
        "${PACKETIA_MONITORING_ROOT}/server/api/MetricsController.cpp"
        "${PACKETIA_MONITORING_ROOT}/server/api/MetricsJsonEncoder.cpp"
        "${PACKETIA_MONITORING_ROOT}/utils/WorkerRegistry.cpp"
        "${PACKETIA_MONITORING_ROOT}/utils/ShardedWorkerPool.cpp"
        "${PACKETIA_MONITORING_ROOT}/utils/MediaLatency.cpp")
    target_include_directories(test_monitor_api_server PRIVATE
        "${PACKETIA_MONITORING_ROOT}/server/api" "${PACKETIA_MONITORING_ROOT}/protocol/http"
        "${PACKETIA_MONITORING_ROOT}/third" "${PACKETIA_MONITORING_ROOT}/utils" "${PACKETIA_MONITORING_ROOT}/log")
    target_link_libraries(test_monitor_api_server PRIVATE packetia_monitor_service packetia_memory)
endif()
add_test(NAME MonitorApiServerTest COMMAND test_monitor_api_server)
set_tests_properties(MonitorApiServerTest PROPERTIES TIMEOUT 20)

cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaWorkerRegistryTests LANGUAGES CXX)
    enable_testing()
endif()

find_package(Threads REQUIRED)
get_filename_component(PACKETIA_WORKERS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_executable(test_worker_registry test_worker_registry.cpp
    "${PACKETIA_WORKERS_ROOT}/server/WorkerSetup.cpp")
target_compile_features(test_worker_registry PRIVATE cxx_std_17)
target_include_directories(test_worker_registry PRIVATE
    "${PACKETIA_WORKERS_ROOT}" "${PACKETIA_WORKERS_ROOT}/utils")

if(TARGET utils)
    target_link_libraries(test_worker_registry PRIVATE utils server)
else()
    # Keep lifecycle tests runnable without the server's FFmpeg, GTest or
    # platform-specific networking dependencies.
    if(NOT TARGET packetia_memory)
        add_subdirectory("${PACKETIA_WORKERS_ROOT}/Common/memory" memory-library)
    endif()
    add_library(worker_registry_test_support STATIC
        "${PACKETIA_WORKERS_ROOT}/server/ServerLauncher.cpp"
        "${PACKETIA_WORKERS_ROOT}/utils/WorkerRegistry.cpp"
        "${PACKETIA_WORKERS_ROOT}/utils/ShardedWorkerPool.cpp"
        "${PACKETIA_WORKERS_ROOT}/utils/EndpointBase.cpp"
        "${PACKETIA_WORKERS_ROOT}/utils/MediaLatency.cpp")
    target_compile_features(worker_registry_test_support PUBLIC cxx_std_17)
    target_include_directories(worker_registry_test_support PRIVATE
        "${PACKETIA_WORKERS_ROOT}"
        "${PACKETIA_WORKERS_ROOT}/log")
    target_link_libraries(worker_registry_test_support PUBLIC packetia_memory Threads::Threads)
    target_link_libraries(test_worker_registry PRIVATE worker_registry_test_support)
endif()

add_test(NAME WorkerRegistryTest COMMAND test_worker_registry)
set_tests_properties(WorkerRegistryTest PROPERTIES
    ENVIRONMENT "PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY=0"
    TIMEOUT 20)

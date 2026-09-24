add_executable(test_state_machine test_state_machine.cpp)
target_compile_features(test_state_machine PRIVATE cxx_std_17)
target_include_directories(test_state_machine PRIVATE ${CMAKE_SOURCE_DIR}/utils)
target_link_libraries(test_state_machine PRIVATE GTest::gtest_main)
add_test(NAME StateMachineUnitTest COMMAND test_state_machine)
set_tests_properties(StateMachineUnitTest PROPERTIES TIMEOUT 10)

add_executable(test_sharded_worker_pool
    test_sharded_worker_pool.cpp
)

target_link_libraries(test_sharded_worker_pool
    PRIVATE
    utils
    LOGGER
    rtp
    GTest::gtest
    GTest::gtest_main
    pthread
)

target_include_directories(test_sharded_worker_pool
    PRIVATE
        ${CMAKE_SOURCE_DIR}/utils
        ${CMAKE_SOURCE_DIR}/Rtsp
        ${CMAKE_SOURCE_DIR}/log
)

add_test(
    NAME ShardedWorkerPoolUnitTest
    COMMAND test_sharded_worker_pool
)

add_executable(test_sharded_worker_lifecycle
    test_sharded_worker_lifecycle.cpp
)

target_link_libraries(test_sharded_worker_lifecycle
    PRIVATE
    utils
    LOGGER
    pthread
)

target_include_directories(test_sharded_worker_lifecycle
    PRIVATE
        ${CMAKE_SOURCE_DIR}/utils
        ${CMAKE_SOURCE_DIR}/log
)

add_test(
    NAME ShardedWorkerLifecycleTest
    COMMAND test_sharded_worker_lifecycle
)

add_subdirectory(workers)
if(APPLE)
    add_subdirectory(scheduler)
endif()

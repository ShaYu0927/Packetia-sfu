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

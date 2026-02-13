add_executable(test_rtp_input
    test_rtp_input.cpp
)

target_link_libraries(test_rtp_input
    PRIVATE
        rtp
        utils
        network
        pthread
        COMMON
)

target_include_directories(test_rtp_input PRIVATE
    ${CMAKE_SOURCE_DIR}/Rtsp
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/Common
)

add_test(NAME test_rtp_input COMMAND test_rtp_input)

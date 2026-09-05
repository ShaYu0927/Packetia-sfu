cmake_minimum_required(VERSION 3.10)
project(h264_packet_buffer_performance_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(h264_packet_buffer_perf
    h264_packet_buffer_perf.cpp
)

target_include_directories(h264_packet_buffer_perf PRIVATE ..)

if(TARGET COMMON)
    target_link_libraries(h264_packet_buffer_perf PRIVATE COMMON)
else()
    target_sources(h264_packet_buffer_perf PRIVATE
        ../H264RtpPayloadParser.cpp
        ../H26xPacketBuffer.cpp
    )
endif()

enable_testing()
add_test(
    NAME h264_packet_buffer_validation
    COMMAND h264_packet_buffer_perf --validate-only
)

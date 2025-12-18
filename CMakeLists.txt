cmake_minimum_required(VERSION 3.10)
project(RtspServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)

# 设置 Debug 模式
set(CMAKE_BUILD_TYPE Debug)

# 添加 AddressSanitizer
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g -O1")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale libavfilter libavdevice)

include_directories(
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/utils
    ${CMAKE_SOURCE_DIR}/log
    ${CMAKE_SOURCE_DIR}/network
    ${CMAKE_SOURCE_DIR}/Rtsp
    ${CMAKE_SOURCE_DIR}/Common
    ${CMAKE_SOURCE_DIR}/protocol/sip
    
)

link_directories(
    ${FFMPEG_LIBRARY_DIRS}
)

add_library(network STATIC
    network/TcpSocket.cpp
    network/SocketUtil.cpp
    network/BufferRead.cpp
    network/BufferWrite.cpp
    network/TcpServer.cpp
    network/TcpConnection.cpp
    network/ProtocolParser.cpp
    utils/Acceptor.cpp
)



add_library(utils STATIC
    utils/Time.cpp
    utils/MemoryManager.cpp
    utils/TaskScheduler.cpp
    utils/SelectTaskScheduler.cpp
    utils/EpollTaskScheduler.cpp
    utils/EventLoop.cpp
    utils/Acceptor.cpp
    utils/Pip.cpp

)

add_library(sip STATIC
    protocol/sip/sip.cpp
    protocol/sip/sip.h
    protocol/sip/SipMessage.cpp
    protocol/sip/SipMessage.h
    protocol/sip/SipParse.cpp
    protocol/sip/SipParse.h
)


add_library(rtp STATIC
    Rtsp/Rtp.cpp
    Rtsp/Rtp.h
    Rtsp/RtpReceiver.cpp
    Rtsp/RtpReceiver.h
    Rtsp/RtpConnection.cpp
    Rtsp/RtpConnection.h
    Rtsp/RtpTypes.h
)

add_library(rtsp STATIC
    Rtsp/Rtsp.cpp
    Rtsp/Rtsp.h
    Rtsp/RtspMessage.cpp
    Rtsp/RtspMessage.h
    Rtsp/RtspServer.cpp
    Rtsp/RtspServer.h
    Rtsp/MediaSession.cpp
    Rtsp/MediaSession.h
    Rtsp/MediaSource.h
    Rtsp/RtspConnection.cpp
    Rtsp/RtspConnection.h
    Rtsp/UdpServer.cpp
    Rtsp/UdpServer.h
    Rtsp/RtpThreadPool.cpp
    Rtsp/RtpThreadPool.h
)

target_link_libraries(rtsp rtp network utils pthread sip)


add_executable(FFmpegAAc main.cpp)
target_link_libraries(FFmpegAAc rtsp rtp utils
    ${FFMPEG_LIBRARIES}
    pthread
    c
)


add_executable(tcp_client test/tcp_client.cpp)
target_link_libraries(tcp_client pthread)

enable_testing()
add_test(NAME TcpClientTest COMMAND tcp_client)

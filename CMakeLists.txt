cmake_minimum_required(VERSION 3.10)
project(Packetia)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) 


if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_definitions(RTP_DEBUG=0)

if (NOT WIN32)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale libavfilter libavdevice)

  add_compile_options(-fno-omit-frame-pointer)
  add_compile_options(-fno-optimize-sibling-calls)
else()

endif()


include_directories(
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/utils
    ${CMAKE_SOURCE_DIR}/log
    ${CMAKE_SOURCE_DIR}/network
    ${CMAKE_SOURCE_DIR}/Rtsp
    ${CMAKE_SOURCE_DIR}/Common
    ${CMAKE_SOURCE_DIR}/src/net
    ${CMAKE_SOURCE_DIR}/protocol/sip
    ${CMAKE_SOURCE_DIR}/protocol/Rtcp
    ${CMAKE_SOURCE_DIR}/protocol/ice
    ${CMAKE_SOURCE_DIR}/protocol/ice/stun
    ${CMAKE_SOURCE_DIR}/src/interleaved
    ${CMAKE_SOURCE_DIR}/Room
    ${CMAKE_SOURCE_DIR}/Common/CallbackEmitter
    ${CMAKE_SOURCE_DIR}/Common/Depacketizer
    
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
    network/TcpSession.cpp
    network/TcpConnection.cpp
    network/ProtocolParser.cpp
    utils/Acceptor.cpp
    network/DefaultSessionFactory.cpp
    network/ProtocolDetector.cpp
    network/UdpSocket.cpp
    network/UdpServer.cpp
    network/UdpSession.cpp
)

add_library(LOGGER STATIC
    log/logger.h
    log/logger.cpp
)

add_library(COMMON STATIC
    Common/CallbackEmitter/CallbackEmitter.h
    Common/CallbackEmitter/CallbackEmitter.cpp
    Common/CallbackEmitter/SourceBase.cpp
    Common/CallbackEmitter/SourceBase.h
    Common/Depacketizer/Depacketizer.h
    Common/Depacketizer/H264Depacketizer.h
    Common/Depacketizer/H264Depacketizer.cpp
)

add_library(utils STATIC
    utils/TimeUtil.cpp
    utils/MemoryManager.cpp
    utils/TaskScheduler.cpp
    utils/SelectTaskScheduler.cpp
    utils/EpollTaskScheduler.cpp
    utils/ShardedWorkerPool.cpp
    utils/PacketPool.cpp
    utils/ShardedWorkerPool.h
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

add_library(Rtcp STATIC
    protocol/Rtcp/Rtcp.cpp
    protocol/Rtcp/Rtcp.h
    protocol/Rtcp/RtcpContext.cpp
    protocol/Rtcp/RtcpContext.h
    protocol/Rtcp/RtcpReciver.h
    protocol/Rtcp/RtcpReciver.cpp
)

add_library(ICE STATIC
    protocol/ice/IceCandidate.cpp
    protocol/ice/stun/Stun.cpp
    protocol/ice/IceCandidateStore.cpp
    protocol/ice/IceChecklist.cpp
    protocol/ice/IceContext.cpp
)

add_library(rtp STATIC
    Rtsp/Rtp.cpp
    Rtsp/Rtp.h
    Rtsp/RtpReceiver.cpp
    Rtsp/RtpReceiver.h
    Rtsp/RtpConnection.cpp
    Rtsp/RtpConnection.h
    Rtsp/RtpTypes.h
    Rtsp/RtspUtil.cpp
    Rtsp/RtspUtil.h
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
    Rtsp/RtpThreadPool.cpp
    Rtsp/RtpThreadPool.h
    Rtsp/RtpInterleaved.cpp
    Rtsp/RtspSession.cpp
    Rtsp/RtspUtil.cpp
)

file(GLOB INTERLEAVED_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interleaved/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.cpp
)

file(GLOB ROOM_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/Room/*.cpp
)

add_library(Interleaved STATIC
    ${INTERLEAVED_SRCS}
)

add_library(ROOM_LIB STATIC
    ${ROOM_SRC}
)


enable_testing()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/Rtsp/Test)


add_executable(Packetia ${CMAKE_SOURCE_DIR}/server/main.cpp)
target_link_libraries(Packetia
    PRIVATE
    -Wl,--start-group
    rtsp rtp network utils Rtcp LOGGER ROOM_LIB COMMON ICE
    -Wl,--end-group
    ${FFMPEG_LIBRARIES}
    pthread
    c
)

add_executable(tcp_client test/tcp_client.cpp)
target_link_libraries(tcp_client pthread)

enable_testing()
add_test(NAME TcpClientTest COMMAND tcp_client)

target_compile_options(Packetia PRIVATE -H)
target_compile_definitions(Packetia PRIVATE RTP_DEBUG=1)
set_target_properties(Packetia PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(Packetia PRIVATE
    -O1
    -g3
    -fno-omit-frame-pointer
)

target_link_options(Packetia PRIVATE
    -fsanitize=address
    -fsanitize=undefined
)

# cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# cmake --build build




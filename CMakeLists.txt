cmake_minimum_required(VERSION 3.10)
project(Packetia)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(LIBHV_DIR ${CMAKE_SOURCE_DIR}/third/libhv)

set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_UNITTEST OFF CACHE BOOL "" FORCE)

set(WITH_EVPP ON CACHE BOOL "" FORCE)
set(WITH_HTTP ON CACHE BOOL "" FORCE)
set(WITH_HTTP_SERVER ON CACHE BOOL "" FORCE)
set(WITH_HTTP_CLIENT OFF CACHE BOOL "" FORCE)

set(WITH_MQTT OFF CACHE BOOL "" FORCE)
set(WITH_CURL OFF CACHE BOOL "" FORCE)
set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third/libhv)


find_package(GTest REQUIRED)
find_package(OpenSSL REQUIRED)

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
    ${CMAKE_SOURCE_DIR}/src/net
    ${CMAKE_SOURCE_DIR}/src/sdp
    ${CMAKE_SOURCE_DIR}/Room
    ${CMAKE_SOURCE_DIR}/Common/CallbackEmitter
    ${CMAKE_SOURCE_DIR}/Common/Depacketizer
    ${CMAKE_SOURCE_DIR}/Common/memory
    ${CMAKE_SOURCE_DIR}/media/endpoint
    ${CMAKE_SOURCE_DIR}/media/track
    ${CMAKE_SOURCE_DIR}/media/MediaFrame
    ${CMAKE_SOURCE_DIR}/server/Moudle
    ${LIBHV_DIR}
    ${LIBHV_DIR}/base
    ${LIBHV_DIR}/cpputil
    ${LIBHV_DIR}/event
    ${LIBHV_DIR}/evpp
    ${LIBHV_DIR}/ssl
    ${LIBHV_DIR}/include
    ${LIBHV_DIR}/http
    ${CMAKE_SOURCE_DIR}/third
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
    network/websocket/WsServer.cpp
    network/websocket/WsSession.cpp
    network/websocket/WsSessionManager.cpp
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

    Common/Depacketizer/H264Payload.h
    Common/Depacketizer/H264RtpPayloadParser.h
    Common/Depacketizer/H264RtpPayloadParser.cpp
    Common/Depacketizer/RtpFrameAssembler.h
    Common/Depacketizer/RtpFrameAssembler.cpp

    Common/Depacketizer/AudioDepacketizer.h
    Common/Depacketizer/AudioDepacketizer.cpp
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
    utils/EndpointBase.cpp
    utils/IWorkerModule.cpp
    utils/utils.cpp

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
    protocol/Rtcp/RtcpHealper.cpp
    protocol/Rtcp/RtcpHealper.h
    protocol/Rtcp/Rtcp.cpp
    protocol/Rtcp/Rtcp.h
    protocol/Rtcp/RtcpContext.cpp
    protocol/Rtcp/RtcpContext.h
    protocol/Rtcp/RtcpReciver.h
    protocol/Rtcp/RtcpReciver.cpp
)

add_library(ICE STATIC
    protocol/ice/IceAgent.cpp
    protocol/ice/IceAgent.h
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
    Rtsp/RtpSenderTrack.cpp
    Rtsp/RtpSenderTrack.h
    Rtsp/RtpConnection.cpp
    Rtsp/RtpConnection.h
    Rtsp/RtpTypes.h
    Rtsp/RtspUtil.cpp
    Rtsp/RtspUtil.h
)

add_library(rtsp STATIC
    Rtsp/Rtsp.h
    Rtsp/RtspMessage.cpp
    Rtsp/RtspMessage.h
    Rtsp/RtspServer.cpp
    Rtsp/RtspServer.h
    Rtsp/RtspMediaSession.cpp
    Rtsp/RtspMediaSession.h
    Rtsp/MediaSource.h
    Rtsp/RtspConnection.cpp
    Rtsp/RtspConnection.h
    Rtsp/RtspSession.cpp
    Rtsp/RtspUtil.cpp
)

file(GLOB INTERLEAVED_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interleaved/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdp/*.cpp
)

file(GLOB ROOM_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/Room/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Room/Transports/*.cpp
)

add_library(Interleaved STATIC
    ${INTERLEAVED_SRCS}
)

add_library(ROOM_LIB STATIC
    ${ROOM_SRC}
)
add_subdirectory(media)
add_subdirectory(server)

enable_testing()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/Rtsp/Test)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/utils/test)


# test

add_executable(ice_unit_tests
    protocol/ice/test/test_IceChecklist.cpp
    protocol/ice/test/test_IceAgent.cpp
)
add_executable(Packetia ${CMAKE_SOURCE_DIR}/server/main.cpp)


target_link_libraries(Packetia
    PRIVATE
    -Wl,--start-group
    media rtsp rtp network utils Rtcp LOGGER ROOM_LIB COMMON ICE Interleaved hv_static 
    -Wl,--end-group
    ${FFMPEG_LIBRARIES}
    pthread
    c
    OpenSSL::Crypto
)

target_link_libraries(ice_unit_tests
    PRIVATE
    ICE
    LOGGER
    GTest::gtest
    GTest::gtest_main
    pthread
    Interleaved
    OpenSSL::Crypto
)

target_include_directories(ice_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/protocol/ice
    ${CMAKE_SOURCE_DIR}/protocol/ice/stun
)
add_test(NAME IceChecklistUnitTest COMMAND ice_unit_tests)
target_compile_options(Packetia PRIVATE -H)
target_compile_definitions(Packetia PRIVATE RTP_DEBUG=1)


# cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# cmake --build build

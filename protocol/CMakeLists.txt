cmake_minimum_required(VERSION 3.16)

add_library(sip STATIC
    sip/sip.cpp
    sip/sip.h
    sip/SipMessage.cpp
    sip/SipMessage.h
    sip/SipParse.cpp
    sip/SipParse.h
    sip/SipSession.cpp
    sip/SipSession.h
    sip/SipServer.cpp
    sip/SipServer.h
)

target_include_directories(sip
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/sip
)

target_compile_features(sip PUBLIC cxx_std_17)

add_library(Rtcp STATIC
    Rtcp/RtcpHealper.cpp
    Rtcp/RtcpHealper.h
    Rtcp/Rtcp.cpp
    Rtcp/Rtcp.h
    Rtcp/RtcpContext.cpp
    Rtcp/RtcpContext.h
    Rtcp/RtcpReciver.h
    Rtcp/RtcpReciver.cpp
    Rtcp/RtcpNack.h
    Rtcp/RtcpNack.cpp
    Rtcp/RtcpFeedback.h
    Rtcp/RtcpFeedback.cpp
)

target_include_directories(Rtcp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Rtcp
)

target_compile_features(Rtcp PUBLIC cxx_std_17)

add_library(ICE STATIC
    ice/IceAgent.cpp
    ice/IceAgent.h
    ice/IceCandidate.cpp
    ice/stun/Stun.cpp
    ice/IceCandidateStore.cpp
    ice/IceChecklist.cpp
    ice/IceContext.cpp
)

target_include_directories(ICE
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/ice
        ${CMAKE_CURRENT_SOURCE_DIR}/ice/stun
)

target_compile_features(ICE PUBLIC cxx_std_17)

add_library(WebRtc STATIC
    WebRtc/webrtc_config.cpp
    WebRtc/webrtc_config.h
    WebRtc/weak_network/NetworkStats.h
)

target_include_directories(WebRtc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/WebRtc
        ${CMAKE_CURRENT_SOURCE_DIR}/WebRtc/weak_network
)

target_compile_features(WebRtc PUBLIC cxx_std_17)

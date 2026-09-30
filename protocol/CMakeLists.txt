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
    Rtcp/TransportFeedbackGenerator.h
    Rtcp/TransportFeedbackGenerator.cpp
)

target_include_directories(Rtcp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Rtcp
)

target_compile_features(Rtcp PUBLIC cxx_std_17)
target_link_libraries(Rtcp PUBLIC utils LOGGER)

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
        ${CMAKE_SOURCE_DIR}/utils
)

target_compile_features(ICE PUBLIC cxx_std_17)
target_link_libraries(ICE PUBLIC LOGGER OpenSSL::Crypto)

add_library(WebRtc STATIC
    WebRtc/WebRtcSession.cpp
    WebRtc/WebRtcSession.h
    WebRtc/WebRtcSdp.h
    WebRtc/WebRtcCodec.cpp
    WebRtc/WebRtcCodec.h
    WebRtc/DtlsTransport.cpp
    WebRtc/DtlsTransport.h
    WebRtc/SrtpTransport.cpp
    WebRtc/SrtpTransport.h
    WebRtc/WebRtcTransport.cpp
    WebRtc/WebRtcTransport.h
    WebRtc/WebRtcUdpMux.cpp
    WebRtc/WebRtcUdpMux.h
    WebRtc/WebRtc_Config.cpp
    WebRtc/WebRtc_Config.h
    WebRtc/weak_network/NetworkStats.h
)

target_include_directories(WebRtc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/WebRtc
        ${CMAKE_CURRENT_SOURCE_DIR}/WebRtc/weak_network
        ${CMAKE_SOURCE_DIR}/media/transport
        ${CMAKE_SOURCE_DIR}/src/sdp
)

target_compile_features(WebRtc PUBLIC cxx_std_17)
target_link_libraries(WebRtc PUBLIC network ICE Interleaved OpenSSL::SSL)

# Existing protocols can still build without SRTP. Enabling WebRTC at runtime
# requires this backend; the service reports a missing dependency on startup.
option(PACKETIA_WITH_SRTP "Enable the libsrtp2 WebRTC media backend" ON)
set(PACKETIA_SRTP_AVAILABLE FALSE PARENT_SCOPE)
if(PACKETIA_WITH_SRTP)
    find_path(PACKETIA_SRTP_INCLUDE_DIR srtp2/srtp.h)
    find_library(PACKETIA_SRTP_LIBRARY NAMES srtp2)
    if(PACKETIA_SRTP_INCLUDE_DIR AND PACKETIA_SRTP_LIBRARY)
        target_include_directories(WebRtc PRIVATE ${PACKETIA_SRTP_INCLUDE_DIR})
        target_link_libraries(WebRtc PUBLIC ${PACKETIA_SRTP_LIBRARY})
        target_compile_definitions(WebRtc PRIVATE PACKETIA_WITH_SRTP=1)
        set(PACKETIA_SRTP_AVAILABLE TRUE PARENT_SCOPE)
    else()
        message(STATUS "libsrtp2 not found: WebRTC runtime enablement will require libsrtp2")
    endif()
endif()

add_library(Rtmp STATIC
    Rtmp/rtmp_message.cpp
    Rtmp/rtmp_chunck.cpp
    Rtmp/rtmp_protocol.cpp
    Rtmp/rtmp_transport.cpp
    Rtmp/rtmp_session.cpp
    Rtmp/rtmp_server.cpp
)
target_include_directories(Rtmp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Rtmp
    ${CMAKE_SOURCE_DIR}/network
)
target_link_libraries(Rtmp PUBLIC network)
target_compile_features(Rtmp PUBLIC cxx_std_17)

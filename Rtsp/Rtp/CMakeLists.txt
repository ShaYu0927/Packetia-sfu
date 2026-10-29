# RTP 参数与 SDP 转换由协议目录维护，不依赖 RTSP 会话或网络对象。
add_library(packetia_rtp_binding STATIC RtpTrackBinding.cpp)
target_include_directories(packetia_rtp_binding PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/media/track)
target_compile_features(packetia_rtp_binding PUBLIC cxx_std_17)
target_link_libraries(packetia_rtp_binding PUBLIC packetia_media_types)

add_library(packetia_sdp_rtp_adapter STATIC SdpTrackBinding.cpp)
target_include_directories(packetia_sdp_rtp_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/sdp)
target_link_libraries(packetia_sdp_rtp_adapter PUBLIC packetia_rtp_binding Interleaved LOGGER)
target_link_libraries(rtp PUBLIC packetia_sdp_rtp_adapter)

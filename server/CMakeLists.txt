cmake_minimum_required(VERSION 3.16)

# Keep the lifecycle controller independent of concrete network/media services.
add_library(server STATIC
    ServerLauncher.cpp
    ServerConfig.cpp
)

target_include_directories(server
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(server PUBLIC cxx_std_17)
target_link_libraries(server PUBLIC LOGGER packetia_config)

if(PACKETIA_WITH_LIBWEBSOCKETS AND PACKETIA_SRTP_AVAILABLE)
    add_executable(PacketiaRoomDemo examples/WebRtcRoomDemo.cpp WebRtcService.cpp)
    target_link_libraries(PacketiaRoomDemo PRIVATE
        "$<$<PLATFORM_ID:Linux>:-Wl,--start-group>"
        server ROOM_LIB media network utils WebRtc TURN LOGGER
        "$<$<PLATFORM_ID:Linux>:-Wl,--end-group>")
endif()

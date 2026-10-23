cmake_minimum_required(VERSION 3.16)
add_subdirectory(api)

# Keep the lifecycle controller independent of concrete network/media services.
add_library(server STATIC
    ServerLauncher.cpp
    ServerConfig.cpp
    StartupConfig.cpp
)

target_include_directories(server
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(server PUBLIC cxx_std_17)
target_link_libraries(server PUBLIC LOGGER packetia_config)

# The optional demo source is not present in every source snapshot.
if(PACKETIA_WITH_LIBWEBSOCKETS AND PACKETIA_SRTP_AVAILABLE AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/WebRtcRoomDemo.cpp")
    add_executable(PacketiaRoomDemo examples/WebRtcRoomDemo.cpp WebRtcService.cpp)
    target_link_libraries(PacketiaRoomDemo PRIVATE
        "$<$<PLATFORM_ID:Linux>:-Wl,--start-group>"
        server ROOM_LIB media network utils WebRtc TURN LOGGER packetia_auth
        "$<$<PLATFORM_ID:Linux>:-Wl,--end-group>")
endif()

if(PACKETIA_WITH_LIBWEBSOCKETS)
    add_library(packetia_cluster_api STATIC cluster/ClusterController.cpp)
    target_link_libraries(packetia_cluster_api PUBLIC packetia_account_api packetia_cluster packetia_auth)
    if(BUILD_TESTING)
        add_executable(test_cluster_api cluster/test/test_cluster_api.cpp)
        target_link_libraries(test_cluster_api PRIVATE packetia_cluster_api GTest::gtest_main)
        add_test(NAME ClusterApiUnitTest COMMAND test_cluster_api)
    endif()
    add_executable(PacketiaControl cluster/ProcessMain.cpp cluster/ControlApp.cpp)
    target_compile_definitions(PacketiaControl PRIVATE PACKETIA_CONTROL_PROCESS=1)
    target_link_libraries(PacketiaControl PRIVATE packetia_cluster_api)
    if(PACKETIA_SRTP_AVAILABLE)
        find_package(CURL REQUIRED)
        add_executable(PacketiaSfu cluster/ProcessMain.cpp cluster/SfuApp.cpp cluster/NodeClient.cpp WebRtcService.cpp)
        target_link_libraries(PacketiaSfu PRIVATE
            "$<$<PLATFORM_ID:Linux>:-Wl,--start-group>"
            server ROOM_LIB media network utils WebRtc LOGGER packetia_auth packetia_cluster CURL::libcurl
            "$<$<PLATFORM_ID:Linux>:-Wl,--end-group>")
        if(BUILD_TESTING)
            find_package(Python3 COMPONENTS Interpreter)
            if(Python3_Interpreter_FOUND)
                add_test(NAME ClusterLauncherUnitTest COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/test_cluster_launcher.py)
                set_tests_properties(ClusterLauncherUnitTest PROPERTIES TIMEOUT 30)
                add_test(NAME ClusterProcessIntegrationTest COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/test_cluster.py
                    --control $<TARGET_FILE:PacketiaControl> --sfu $<TARGET_FILE:PacketiaSfu>)
                set_tests_properties(ClusterProcessIntegrationTest PROPERTIES TIMEOUT 90)
            endif()
        endif()
    endif()
endif()

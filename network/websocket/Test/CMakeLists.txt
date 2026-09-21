cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaWebSocketTests LANGUAGES C CXX)
    enable_testing()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The project's EventLoop uses epoll and pipe2; standalone WebSocket tests require Linux")
    endif()
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(Threads REQUIRED)
    get_filename_component(PACKETIA_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    add_library(utils STATIC
        "${PACKETIA_SOURCE_ROOT}/utils/TaskScheduler.cpp"
        "${PACKETIA_SOURCE_ROOT}/utils/MediaLatency.cpp"
        "${PACKETIA_SOURCE_ROOT}/utils/EpollTaskScheduler.cpp"
        "${PACKETIA_SOURCE_ROOT}/utils/EventLoop.cpp"
        "${PACKETIA_SOURCE_ROOT}/utils/Pip.cpp"
        "${PACKETIA_SOURCE_ROOT}/utils/TimeUtil.cpp"
        "${PACKETIA_SOURCE_ROOT}/network/SocketUtil.cpp")
    target_include_directories(utils PUBLIC
        "${PACKETIA_SOURCE_ROOT}/utils"
        "${PACKETIA_SOURCE_ROOT}/network"
        "${PACKETIA_SOURCE_ROOT}/log")
    target_compile_features(utils PUBLIC cxx_std_17)
    target_link_libraries(utils PUBLIC Threads::Threads)
    add_subdirectory("${PACKETIA_SOURCE_ROOT}/network/websocket"
        "${CMAKE_CURRENT_BINARY_DIR}/websocket")
endif()

add_executable(test_ws_session test_ws_session.cpp)
target_link_libraries(test_ws_session PRIVATE packetia_websocket Threads::Threads)
add_test(NAME test_ws_session COMMAND test_ws_session)
set_tests_properties(test_ws_session PROPERTIES TIMEOUT 30)

add_executable(test_ws_server test_ws_server.cpp)
target_link_libraries(test_ws_server PRIVATE packetia_websocket Threads::Threads)
add_test(NAME test_ws_server COMMAND test_ws_server)
set_tests_properties(test_ws_server PROPERTIES TIMEOUT 60)

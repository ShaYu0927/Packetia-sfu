if(NOT TARGET packetia_libwebsockets)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../cmake/libwebsockets"
        "${CMAKE_CURRENT_BINARY_DIR}/libwebsockets")
endif()
find_package(Threads REQUIRED)

add_library(packetia_websocket STATIC
    WsServer.cpp
    WsSession.cpp
    WsSessionManager.cpp)
target_compile_features(packetia_websocket PUBLIC cxx_std_17)
target_compile_definitions(packetia_websocket PUBLIC PACKETIA_WITH_LIBWEBSOCKETS=1)
target_include_directories(packetia_websocket PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/.."
    "${CMAKE_CURRENT_LIST_DIR}/../../utils"
    "${CMAKE_CURRENT_LIST_DIR}/../../log")
target_link_libraries(packetia_websocket PUBLIC
    packetia_libwebsockets utils Threads::Threads)

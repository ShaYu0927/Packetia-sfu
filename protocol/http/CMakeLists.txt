add_library(packetia_http STATIC
    HttpServerOptions.h
    HttpMessage.h
    IHttpServer.h
    HttpServer.h
    HttpServer.cpp
)

target_compile_features(packetia_http PUBLIC cxx_std_17)
target_include_directories(packetia_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(packetia_http PRIVATE utils LOGGER)
if(PACKETIA_WITH_LIBWEBSOCKETS)
    target_compile_definitions(packetia_http PRIVATE PACKETIA_HTTP_WITH_LIBWEBSOCKETS=1)
    target_link_libraries(packetia_http PRIVATE packetia_libwebsockets)
    if(BUILD_TESTING)
        add_executable(test_http_server test/test_http_server.cpp)
        target_link_libraries(test_http_server PRIVATE packetia_http utils GTest::gtest_main)
        add_test(NAME HttpServerIntegrationTest COMMAND test_http_server)
        set_tests_properties(HttpServerIntegrationTest PROPERTIES TIMEOUT 30)
    endif()
endif()

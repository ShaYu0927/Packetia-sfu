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
    # LWS_WITH_CUSTOM_HEADERS also exists in older releases that cannot
    # enumerate custom header names (including Debian bookworm's 4.1).
    include(CheckCXXSymbolExists)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    if(TARGET PkgConfig::PACKETIA_LWS)
        set(CMAKE_REQUIRED_LIBRARIES PkgConfig::PACKETIA_LWS)
        if(TARGET OpenSSL::SSL)
            list(APPEND CMAKE_REQUIRED_LIBRARIES OpenSSL::SSL)
        endif()
    else()
        set(CMAKE_REQUIRED_LIBRARIES packetia_libwebsockets)
    endif()
    unset(PACKETIA_LWS_HAS_CUSTOM_HEADER_ENUMERATION CACHE)
    check_cxx_symbol_exists(lws_hdr_custom_name_foreach "libwebsockets.h"
        PACKETIA_LWS_HAS_CUSTOM_HEADER_ENUMERATION)
    cmake_pop_check_state()
    if(PACKETIA_LWS_HAS_CUSTOM_HEADER_ENUMERATION)
        target_compile_definitions(packetia_http PRIVATE PACKETIA_LWS_HAS_CUSTOM_HEADER_ENUMERATION=1)
    endif()
    if(BUILD_TESTING)
        add_executable(test_http_server test/test_http_server.cpp)
        target_link_libraries(test_http_server PRIVATE packetia_http utils GTest::gtest_main)
        add_test(NAME HttpServerIntegrationTest COMMAND test_http_server)
        set_tests_properties(HttpServerIntegrationTest PROPERTIES TIMEOUT 30
            ENVIRONMENT "PACKETIA_LOG_MODE=console;PACKETIA_LOG_FORMAT=json;PACKETIA_LOG_LEVEL=info")
    endif()
endif()

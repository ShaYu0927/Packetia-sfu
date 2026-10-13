add_library(packetia_account_api STATIC
    AccountController.cpp
    AccountController.h
    ApiServer.cpp
    ApiServer.h
    ../../network/http/HttpOptions.h
    ../../network/http/HttpTypes.h
    ../../network/http/HttpServer.h
    ../../network/http/LibwebsocketsHttpServer.h
    ../../network/http/LibwebsocketsHttpServer.cpp
)

target_compile_features(packetia_account_api PUBLIC cxx_std_17)
target_include_directories(packetia_account_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(packetia_account_api PUBLIC packetia_account utils)

if(BUILD_TESTING)
    add_executable(test_account_api test/test_account_api.cpp)
    target_link_libraries(test_account_api PRIVATE packetia_account_api GTest::gtest_main)
    add_test(NAME AccountApiFrameworkUnitTest COMMAND test_account_api)
endif()

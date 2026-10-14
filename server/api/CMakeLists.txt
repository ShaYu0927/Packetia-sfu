add_library(packetia_account_api STATIC
    ApiRouter.cpp
    ApiRouter.h
    ApiResponse.cpp
    ApiResponse.h
    AccountController.cpp
    AccountController.h
    ApiServer.cpp
    ApiServer.h
)

target_compile_features(packetia_account_api PUBLIC cxx_std_17)
target_include_directories(packetia_account_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(packetia_account_api PUBLIC packetia_account packetia_http utils)

if(BUILD_TESTING)
    add_executable(test_account_api test/test_account_api.cpp)
    target_link_libraries(test_account_api PRIVATE packetia_account_api GTest::gtest_main)
    add_test(NAME AccountApiFrameworkUnitTest COMMAND test_account_api)
    add_executable(test_api_router test/test_api_router.cpp)
    target_link_libraries(test_api_router PRIVATE packetia_account_api GTest::gtest_main)
    add_test(NAME ApiRouterUnitTest COMMAND test_api_router)
    set_tests_properties(ApiRouterUnitTest PROPERTIES TIMEOUT 15)
endif()

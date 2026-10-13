add_library(packetia_auth STATIC
    AuthTypes.h
    Authenticator.h
    AuthAccessPolicy.h
    JwtAuthenticator.h
    JwtAuthenticator.cpp
    KeyProvider.h
    RoomAccessPolicy.h
    RoomAccessPolicy.cpp
    AuthService.h
    AuthService.cpp
)

target_compile_features(packetia_auth PUBLIC cxx_std_17)
target_include_directories(packetia_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(packetia_auth PUBLIC OpenSSL::Crypto)

if(BUILD_TESTING)
    add_executable(test_auth_framework test/test_auth_framework.cpp test/test_jwt_authenticator.cpp)
    target_link_libraries(test_auth_framework PRIVATE packetia_auth GTest::gtest_main)
    add_test(NAME AuthFrameworkUnitTest COMMAND test_auth_framework)
endif()

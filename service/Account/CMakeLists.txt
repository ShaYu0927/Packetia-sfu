if(NOT TARGET SQLite::SQLite3 AND NOT TARGET SQLite3::SQLite3)
    find_package(SQLite3 3.24 REQUIRED)
endif()

add_library(packetia_account STATIC
    AccountPolicy.h
    AccountOptions.h
    AccountTypes.h
    AccountStore.h
    PasswordHasher.h
    ScryptPasswordHasher.h
    ScryptPasswordHasher.cpp
    SqliteAccountStore.h
    SqliteAccountStore.cpp
    AccountService.h
    AccountService.cpp
)

target_compile_features(packetia_account PUBLIC cxx_std_17)
target_include_directories(packetia_account PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(packetia_account PUBLIC Threads::Threads OpenSSL::Crypto)
if(TARGET SQLite3::SQLite3)
    target_link_libraries(packetia_account PUBLIC SQLite3::SQLite3)
else()
    target_link_libraries(packetia_account PUBLIC SQLite::SQLite3)
endif()

if(BUILD_TESTING)
    add_executable(test_account_framework test/test_account_framework.cpp)
    target_link_libraries(test_account_framework PRIVATE packetia_account GTest::gtest_main)
    add_test(NAME AccountFrameworkUnitTest COMMAND test_account_framework)
    add_executable(test_account_business test/test_account_business.cpp)
    target_link_libraries(test_account_business PRIVATE packetia_account GTest::gtest_main)
    add_test(NAME AccountBusinessUnitTest COMMAND test_account_business)
    set_tests_properties(AccountBusinessUnitTest PROPERTIES TIMEOUT 60)
endif()

add_library(packetia_account STATIC
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
target_link_libraries(packetia_account PUBLIC Threads::Threads)

if(BUILD_TESTING)
    add_executable(test_account_framework test/test_account_framework.cpp)
    target_link_libraries(test_account_framework PRIVATE packetia_account GTest::gtest_main)
    add_test(NAME AccountFrameworkUnitTest COMMAND test_account_framework)
endif()

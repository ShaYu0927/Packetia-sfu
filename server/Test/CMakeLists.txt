cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaServerTests LANGUAGES CXX)
    enable_testing()
    find_package(Threads REQUIRED)
    get_filename_component(PACKETIA_SERVER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    add_library(server_test_support STATIC
        "${PACKETIA_SERVER_ROOT}/server/ServerLauncher.cpp")
    target_include_directories(server_test_support PUBLIC
        "${PACKETIA_SERVER_ROOT}"
        "${PACKETIA_SERVER_ROOT}/log")
    target_compile_features(server_test_support PUBLIC cxx_std_17)
    target_link_libraries(server_test_support PUBLIC Threads::Threads)
    set(PACKETIA_SERVER_TEST_LIBRARY server_test_support)
else()
    set(PACKETIA_SERVER_TEST_LIBRARY server)
endif()

add_executable(test_server_launcher test_server_launcher.cpp)
target_link_libraries(test_server_launcher PRIVATE ${PACKETIA_SERVER_TEST_LIBRARY})
add_test(NAME server_launcher COMMAND test_server_launcher)
set_tests_properties(server_launcher PROPERTIES TIMEOUT 20)

cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaConferenceMixTests LANGUAGES CXX)
    enable_testing()
endif()

find_package(Threads REQUIRED)
get_filename_component(PACKETIA_MIX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(NOT TARGET packetia_config)
    add_subdirectory("${PACKETIA_MIX_ROOT}/config" "${CMAKE_CURRENT_BINARY_DIR}/config")
endif()
add_executable(conference_mix_tests
    test_conference_mixer.cpp
    ../ConferenceMixer.cpp
    ../ConferenceMixService.cpp
    ${PACKETIA_MIX_ROOT}/server/ServerLauncher.cpp
    ${PACKETIA_MIX_ROOT}/media/core/EncodedFrameRouter.cpp
    ${PACKETIA_MIX_ROOT}/log/logger.cpp
)
target_include_directories(conference_mix_tests PRIVATE
    ${PACKETIA_MIX_ROOT} ${PACKETIA_MIX_ROOT}/log)
target_compile_features(conference_mix_tests PRIVATE cxx_std_17)
target_link_libraries(conference_mix_tests PRIVATE Threads::Threads packetia_config)
add_test(NAME ConferenceMixFramework COMMAND conference_mix_tests)
set_tests_properties(ConferenceMixFramework PROPERTIES TIMEOUT 30)

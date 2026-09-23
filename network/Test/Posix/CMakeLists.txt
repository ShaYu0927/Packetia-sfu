cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaPosixSocketTests LANGUAGES CXX)
    enable_testing()
endif()

find_package(Threads REQUIRED)
get_filename_component(PACKETIA_POSIX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET packetia_memory)
    add_subdirectory("${PACKETIA_POSIX_ROOT}/Common/memory" memory-library)
endif()

add_executable(test_posix_socket
    test_posix_socket.cpp
    "${PACKETIA_POSIX_ROOT}/network/TcpSocket.cpp"
    "${PACKETIA_POSIX_ROOT}/network/SocketUtil.cpp"
    "${PACKETIA_POSIX_ROOT}/network/BufferWrite.cpp"
    "${PACKETIA_POSIX_ROOT}/utils/Pip.cpp"
    "${PACKETIA_POSIX_ROOT}/utils/MediaLatency.cpp")
target_compile_features(test_posix_socket PRIVATE cxx_std_17)
target_include_directories(test_posix_socket PRIVATE
    "${PACKETIA_POSIX_ROOT}/network"
    "${PACKETIA_POSIX_ROOT}/utils"
    "${PACKETIA_POSIX_ROOT}/log")
target_link_libraries(test_posix_socket PRIVATE packetia_memory Threads::Threads)

add_test(NAME posix_socket COMMAND test_posix_socket)
set_tests_properties(posix_socket PROPERTIES TIMEOUT 20)

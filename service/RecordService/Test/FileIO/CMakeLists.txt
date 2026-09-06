cmake_minimum_required(VERSION 3.16)
project(FileIOTests LANGUAGES CXX)
enable_testing()

# Can also be configured on its own, without FFmpeg, libmov or GTest.
add_executable(test_local_file_io test_local_file_io.cpp ../../LocalFileIO.cpp)
target_include_directories(test_local_file_io PRIVATE ../..)
target_compile_features(test_local_file_io PRIVATE cxx_std_11)
add_test(NAME test_local_file_io COMMAND test_local_file_io
    "${CMAKE_CURRENT_BINARY_DIR}/local-file-io-test.bin")

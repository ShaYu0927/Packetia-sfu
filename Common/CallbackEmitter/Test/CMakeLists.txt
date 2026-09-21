cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaCallbackEmitterTests LANGUAGES CXX)
    enable_testing()
endif()

find_package(Threads REQUIRED)

add_executable(test_callback_emitter test_callback_emitter.cpp)
target_compile_features(test_callback_emitter PRIVATE cxx_std_17)
target_include_directories(test_callback_emitter PRIVATE ..)
target_link_libraries(test_callback_emitter PRIVATE Threads::Threads)

add_test(NAME callback_emitter COMMAND test_callback_emitter)
set_tests_properties(callback_emitter PROPERTIES TIMEOUT 20)

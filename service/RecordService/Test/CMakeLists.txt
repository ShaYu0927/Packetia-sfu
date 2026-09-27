cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RecordingTests LANGUAGES CXX)
    enable_testing()
    find_package(Threads REQUIRED)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../config" "${CMAKE_CURRENT_BINARY_DIR}/config")
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/ffmpeg"
        "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg")
endif()

add_subdirectory(FileIO)
add_subdirectory(../Index recording-index)
unset(RECORDING_FFMPEG)
unset(RECORDING_FFMPEG CACHE)
find_program(RECORDING_FFMPEG NAMES ffmpeg)
if(RECORDING_FFMPEG)
    set(RECORDING_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/recording-fixture.mp4")
    add_test(NAME recording_fixture COMMAND ${RECORDING_FFMPEG} -hide_banner -loglevel error -y
        -f lavfi -i testsrc2=size=160x120:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100
        -t 2 -c:v libx264 -preset ultrafast -bf 0 -g 25 -pix_fmt yuv420p -c:a aac -ac 2
        ${RECORDING_FIXTURE})
    set_tests_properties(recording_fixture PROPERTIES FIXTURES_SETUP recording_media TIMEOUT 30)
    # Exercise the actual recorder and muxer without the unrelated network stack.
    add_executable(test_recording test_recording.cpp
        ../Mp4Recorder.cpp ../Mp4Writer.cpp ../LocalFileIO.cpp)
    target_compile_features(test_recording PRIVATE cxx_std_17)
    target_include_directories(test_recording PRIVATE
        .. ../../../media/core ../../../log ../../../utils ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(test_recording PRIVATE packetia_recording_index ${FFMPEG_LIBRARIES} Threads::Threads)
    if(MSVC)
        target_compile_options(test_recording PRIVATE /utf-8)
    endif()
    add_test(NAME test_recording COMMAND test_recording "${RECORDING_FIXTURE}")
    set_tests_properties(test_recording PROPERTIES FIXTURES_REQUIRED recording_media TIMEOUT 30)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(PACKETIA_RECORD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    add_library(recording_test_pipeline STATIC
        ../RecordingService.cpp ../RecordingDispatcher.cpp ../RecordingSession.cpp ../RecordingInstance.cpp
        ../Mp4Recorder.cpp ../Mp4Writer.cpp ../LocalFileIO.cpp
        ${PACKETIA_RECORD_ROOT}/media/core/EncodedFrameRouter.cpp
        ${PACKETIA_RECORD_ROOT}/utils/WorkerRegistry.cpp
        ${PACKETIA_RECORD_ROOT}/utils/ShardedWorkerPool.cpp
        ${PACKETIA_RECORD_ROOT}/utils/MediaLatency.cpp)
    target_include_directories(recording_test_pipeline PUBLIC
        .. ${PACKETIA_RECORD_ROOT} ${PACKETIA_RECORD_ROOT}/service ${PACKETIA_RECORD_ROOT}/media
        ${PACKETIA_RECORD_ROOT}/media/core ${PACKETIA_RECORD_ROOT}/log ${PACKETIA_RECORD_ROOT}/utils)
    target_link_libraries(recording_test_pipeline PUBLIC packetia_config packetia_recording_index ${FFMPEG_LIBRARIES} Threads::Threads)
    add_executable(test_recording_control test_recording_control.cpp)
    target_link_libraries(test_recording_control PRIVATE recording_test_pipeline)
    add_test(NAME recording_control COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_LIST_DIR}/verify_recording_control.py
        $<TARGET_FILE:test_recording_control> ${RECORDING_FIXTURE} ${RECORDING_FFMPEG}
        ${CMAKE_CURRENT_BINARY_DIR}/recording-control-artifacts)
    set_tests_properties(recording_control PROPERTIES FIXTURES_REQUIRED recording_media TIMEOUT 150)
    add_executable(test_segmented_recording test_segmented_recording.cpp)
    target_link_libraries(test_segmented_recording PRIVATE recording_test_pipeline)
    add_test(NAME segmented_recording COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_LIST_DIR}/verify_segmented_recording.py
        $<TARGET_FILE:test_segmented_recording> ${RECORDING_FIXTURE} ${RECORDING_FFMPEG}
        ${CMAKE_CURRENT_BINARY_DIR}/segmented-recording-artifacts)
    set_tests_properties(segmented_recording PROPERTIES FIXTURES_REQUIRED recording_media TIMEOUT 150)
else()
    message(FATAL_ERROR
        "Recording integration tests require ffmpeg on PATH with libx264 and AAC encoding support. "
        "Install the system ffmpeg package or disable PACKETIA_BUILD_RECORDING_TESTS.")
endif()

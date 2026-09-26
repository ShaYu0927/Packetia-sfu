cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RecordingTests LANGUAGES CXX)
    enable_testing()
    find_package(Threads REQUIRED)
    set(PACKETIA_RECORDING_FFMPEG_ROOT "" CACHE PATH "FFmpeg development installation")
    if(PACKETIA_RECORDING_FFMPEG_ROOT)
        find_path(FFMPEG_INCLUDE_DIRS libavformat/avformat.h
            PATHS "${PACKETIA_RECORDING_FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
        foreach(component avformat avcodec avutil)
            find_library(RECORDING_${component}_LIBRARY NAMES ${component}
                PATHS "${PACKETIA_RECORDING_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
            if(NOT RECORDING_${component}_LIBRARY)
                message(FATAL_ERROR "Missing FFmpeg library: ${component}")
            endif()
            list(APPEND FFMPEG_LIBRARIES "${RECORDING_${component}_LIBRARY}")
        endforeach()
        if(NOT FFMPEG_INCLUDE_DIRS)
            message(FATAL_ERROR "Missing FFmpeg development headers")
        endif()
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
        set(FFMPEG_LIBRARIES PkgConfig::FFMPEG)
    endif()
endif()

add_subdirectory(FileIO)
find_program(RECORDING_FFMPEG ffmpeg HINTS "${PACKETIA_RECORDING_FFMPEG_ROOT}/bin")
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
    target_link_libraries(test_recording PRIVATE ${FFMPEG_LIBRARIES} Threads::Threads)
    if(MSVC)
        target_compile_options(test_recording PRIVATE /utf-8)
    endif()
    add_test(NAME test_recording COMMAND test_recording "${RECORDING_FIXTURE}")
    set_tests_properties(test_recording PROPERTIES FIXTURES_REQUIRED recording_media TIMEOUT 30)
endif()

add_subdirectory(FileIO)
find_program(RECORDING_FFMPEG ffmpeg)
if(RECORDING_FFMPEG)
    set(RECORDING_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/recording-fixture.mp4")
    add_test(NAME recording_fixture COMMAND ${RECORDING_FFMPEG} -hide_banner -loglevel error -y
        -f lavfi -i testsrc2=size=160x120:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100
        -t 2 -c:v libx264 -preset ultrafast -bf 0 -g 25 -pix_fmt yuv420p -c:a aac -ac 2
        ${RECORDING_FIXTURE})
    set_tests_properties(recording_fixture PROPERTIES FIXTURES_SETUP recording_media TIMEOUT 30)
    add_executable(test_recording test_recording.cpp)
    target_compile_definitions(test_recording PRIVATE RECORDING_FIXTURE="${RECORDING_FIXTURE}")
    target_link_libraries(test_recording PRIVATE -Wl,--start-group service media rtsp rtp network utils Rtcp LOGGER ROOM_LIB COMMON ICE Interleaved -Wl,--end-group
        ${FFMPEG_LIBRARIES} GTest::gtest GTest::gtest_main OpenSSL::Crypto pthread)
    add_test(NAME test_recording COMMAND test_recording)
    set_tests_properties(test_recording PROPERTIES FIXTURES_REQUIRED recording_media TIMEOUT 30)
endif()

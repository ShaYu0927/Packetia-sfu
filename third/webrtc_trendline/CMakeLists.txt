cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PacketiaWebRtcTrendline LANGUAGES CXX)
    include(CTest)
    if(BUILD_TESTING)
        find_package(GTest REQUIRED)
    endif()
endif()

add_library(packetia_webrtc_trendline STATIC port/trendline_estimator.cc)
target_compile_features(packetia_webrtc_trendline PUBLIC cxx_std_17)
target_include_directories(packetia_webrtc_trendline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(BUILD_TESTING)
    # Build the untouched upstream implementation as a separate reference.
    # Compatibility headers support only empty field trials and no predictor.
    add_executable(test_webrtc_trendline
        upstream/modules/congestion_controller/goog_cc/trendline_estimator.cc
        upstream/modules/congestion_controller/goog_cc/trendline_estimator_unittest.cc
        tests/test_port_equivalence.cpp)
    target_include_directories(test_webrtc_trendline PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/upstream ${CMAKE_CURRENT_SOURCE_DIR}/tests/compat)
    target_link_libraries(test_webrtc_trendline PRIVATE packetia_webrtc_trendline GTest::gtest_main)
    add_test(NAME webrtc_trendline COMMAND test_webrtc_trendline)
    set_tests_properties(webrtc_trendline PROPERTIES TIMEOUT 15)
endif()

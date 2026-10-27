if(NOT TARGET packetia_video_codecs)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../media/codec
        ${CMAKE_CURRENT_BINARY_DIR}/video-codecs)
endif()

add_library(packetia_transcode_domain STATIC TranscodeTask.cpp TranscodeTypes.cpp)
target_include_directories(packetia_transcode_domain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/codec
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/MediaFrame
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/types)
target_compile_features(packetia_transcode_domain PUBLIC cxx_std_17)

add_library(packetia_video_transcoder STATIC VideoTranscoder.cpp)
target_include_directories(packetia_video_transcoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/MediaFrame
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/types)
target_compile_features(packetia_video_transcoder PUBLIC cxx_std_17)
target_link_libraries(packetia_video_transcoder PUBLIC
    packetia_transcode_domain packetia_video_codecs)

add_library(packetia_transcode_service STATIC TranscodeSession.cpp TranscodeService.cpp)
target_include_directories(packetia_transcode_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/core)
target_link_libraries(packetia_transcode_service PUBLIC packetia_video_transcoder packetia_threads)
if(TARGET media)
    target_link_libraries(packetia_transcode_service PUBLIC media)
endif()

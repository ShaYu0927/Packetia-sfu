add_library(packetia_video_codecs STATIC FfmpegVideoCodec.cpp VideoCodecFactory.cpp)
target_include_directories(packetia_video_codecs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../frame
    ${CMAKE_CURRENT_SOURCE_DIR}/../types)
target_compile_features(packetia_video_codecs PUBLIC cxx_std_17)
target_link_libraries(packetia_video_codecs
    PUBLIC packetia_memory
    PRIVATE packetia_ffmpeg)

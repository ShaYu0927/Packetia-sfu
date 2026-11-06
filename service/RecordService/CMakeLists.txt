add_subdirectory(Index)

add_library(packetia_recording_service STATIC
    RecordingService.cpp
    RecordingInstance.cpp
    RecordingSession.cpp
    Mp4Recorder.cpp
    Mp4Writer.cpp
    Mp4Reader.cpp
    LocalFileIO.cpp
    RecordingDispatcher.cpp)
target_compile_features(packetia_recording_service PUBLIC cxx_std_17)
target_include_directories(packetia_recording_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(packetia_recording_service
    PUBLIC packetia_processing_api packetia_config packetia_media_core
    PRIVATE packetia_recording_index utils LOGGER packetia_ffmpeg)

if(MSVC)
    target_compile_options(packetia_recording_service PRIVATE /utf-8)
endif()

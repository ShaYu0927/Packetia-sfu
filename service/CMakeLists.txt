cmake_minimum_required(VERSION 3.16)

add_library(service STATIC
    core/IService.h
    core/ServiceBase.h
    core/ServiceManager.h
    core/ServiceManager.cpp
    AIService/AITypes.h
    AIService/IModelProvider.h
    AIService/UnavailableModelProvider.h
    AIService/UnavailableModelProvider.cpp
    AIService/AIOrchestrator.h
    AIService/AIOrchestrator.cpp
    AIService/AIFrameIngress.h
    AIService/AIFrameIngress.cpp
    AIService/AIService.h
    AIService/AIService.cpp
    RecordService/RecordingService.h
    RecordService/RecordingService.cpp
    RecordService/RecordingOptions.h
    RecordService/Mp4Recorder.h
    RecordService/Mp4Recorder.cpp
    RecordService/Mp4Writer.cpp
    RecordService/ISeekableFile.h
    RecordService/LocalFileIO.h
    RecordService/LocalFileIO.cpp
    RecordService/RecordingDispatcher.h
    RecordService/RecordingDispatcher.cpp
)

target_include_directories(service
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/AIService
        ${CMAKE_CURRENT_SOURCE_DIR}/RecordService
)

target_compile_features(service PUBLIC cxx_std_17)

target_link_libraries(service PUBLIC media LOGGER ${FFMPEG_LIBRARIES})

if(MSVC)
    target_compile_options(service PRIVATE /utf-8)
endif()

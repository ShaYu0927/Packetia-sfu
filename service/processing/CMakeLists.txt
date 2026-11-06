# Contracts expose headers and shared frame types, without processing backends.
add_library(packetia_processing_api INTERFACE)
target_compile_features(packetia_processing_api INTERFACE cxx_std_17)
target_include_directories(packetia_processing_api INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../RecordService
    ${CMAKE_CURRENT_SOURCE_DIR}/../ConferenceMixService
    ${CMAKE_CURRENT_SOURCE_DIR}/../TranscodeService
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/codec
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../media/frame)
target_link_libraries(packetia_processing_api INTERFACE packetia_media_types packetia_memory)

# The executable selects local composition. Implementations are link dependencies,
# not public header requirements for callers using only the service interfaces.
add_library(packetia_local_processing STATIC MediaProcessingServices.cpp)
target_compile_features(packetia_local_processing PUBLIC cxx_std_17)
target_link_libraries(packetia_local_processing
    PUBLIC packetia_processing_api
    PRIVATE packetia_recording_service packetia_ai_service
            packetia_conference_mix_service packetia_transcode_service)

cmake_minimum_required(VERSION 3.16)
add_subdirectory(core)
add_subdirectory(Auth)
add_subdirectory(Account)
add_subdirectory(Cluster)
add_subdirectory(MonitorService)
add_subdirectory(processing)
add_subdirectory(AIService)
add_subdirectory(RecordService)
add_subdirectory(ConferenceMixService)
add_subdirectory(TranscodeService)
add_subdirectory(meeting)

# Compatibility aggregate. New consumers select the components they actually use.
add_library(service INTERFACE)
target_link_libraries(service INTERFACE
    packetia_service_core
    packetia_ai_service
    packetia_recording_service
    packetia_conference_mix_service
    packetia_transcode_service)

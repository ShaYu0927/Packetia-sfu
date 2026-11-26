add_library(packetia_ai_service STATIC
    UnavailableModelProvider.cpp
    AIOrchestrator.cpp
    AIFrameIngress.cpp
    AIService.cpp)
target_compile_features(packetia_ai_service PUBLIC cxx_std_17)
target_include_directories(packetia_ai_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(packetia_ai_service
    PUBLIC packetia_processing_api packetia_config packetia_media_core packetia_threads
    PRIVATE LOGGER)

# 语音会话控制属于 AI 模块，单独构建供 Control 使用。
add_library(packetia_agent_service STATIC VoiceAgentService.cpp)
target_compile_features(packetia_agent_service PUBLIC cxx_std_17)
target_include_directories(packetia_agent_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(packetia_agent_service PUBLIC packetia_meeting)
if(PACKETIA_WITH_GRPC)
    target_link_libraries(packetia_agent_service PRIVATE packetia_grpc)
endif()

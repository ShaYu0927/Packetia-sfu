add_library(packetia_config STATIC AppConfig.cpp ConfigStore.cpp)
target_include_directories(packetia_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(packetia_config PUBLIC cxx_std_17)
target_link_libraries(packetia_config PUBLIC Threads::Threads)

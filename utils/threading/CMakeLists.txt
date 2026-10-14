add_library(packetia_threads STATIC ../ThreadManager.cpp)
target_compile_features(packetia_threads PUBLIC cxx_std_17)
target_include_directories(packetia_threads PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
target_link_libraries(packetia_threads PUBLIC Threads::Threads)

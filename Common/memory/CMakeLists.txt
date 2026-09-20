add_library(packetia_memory STATIC
    MemoryPool.cpp
    ThreadCache.cpp
    CentralCache.cpp)
target_compile_features(packetia_memory PUBLIC cxx_std_17)
target_link_libraries(packetia_memory PUBLIC Threads::Threads)

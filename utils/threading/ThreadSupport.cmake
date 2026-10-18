# Reuse the root project's thread library, or create it for standalone tests.
if(NOT TARGET packetia_threads)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/packetia-threading")
endif()

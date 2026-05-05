cmake_minimum_required(VERSION 3.16)

file(GLOB SERVER_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/Moudle/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(server STATIC
    ${SERVER_SRC}
)

target_include_directories(server
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(server PUBLIC cxx_std_17)
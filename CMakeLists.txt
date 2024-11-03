cmake_minimum_required(VERSION 3.10)
project(FFmpegAAc)

set(CMAKE_CXX_STANDARD 17)

set(FFMPEG_ROOT "/usr/local/ffmpeg")
set(PKG_CONFIG_PATH "${FFMPEG_ROOT}/lib/pkgconfig")

# 源文件列表
set(FILES
    log/KXY_logger.cpp
    log/KXY_logger.h
    log/KXY_Message.h
    network/Socket.h
    network/TcpSocket.cpp
    network/TcpSocket.h
    network/SocketUtil.cpp
    network/SocketUtil.h
    network/BufferWrite.h
    network/BufferWrite.cpp
    utils/ThreadSafeQueue.h
    utils/Time.cpp
    utils/Time.h
    utils/MemoryManager.h
    utils/MemoryManager.cpp
)


find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(AVFILTER REQUIRED libavfilter)
pkg_check_modules(AVDEVICE REQUIRED libavdevice)

link_directories(${OUT_NETWORK})

include_directories(
    "${FFMPEG_ROOT}/include"
    ${CMAKE_SOURCE_DIR}/utils
    ${CMAKE_SOURCE_DIR}/log
    ${CMAKE_SOURCE_DIR}/network
)


add_executable(FFmpegAAc main.cpp ${FILES})

link_directories(
    ${AVCODEC_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${AVFILTER_LIBRARY_DIRS}
    ${AVDEVICE_LIBRARY_DIRS}
)

# 链接库
target_link_libraries(FFmpegAAc
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AVFILTER_LIBRARIES}
    ${AVDEVICE_LIBRARIES}
    pthread
    c
)




# Use system FFmpeg development packages for the application and recording tests.
# The imported target propagates the pkg-config include paths and link options.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PACKETIA_SYSTEM_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
    libavformat libavcodec libavutil)

add_library(packetia_ffmpeg INTERFACE)
target_link_libraries(packetia_ffmpeg INTERFACE PkgConfig::PACKETIA_SYSTEM_FFMPEG)

set(FFMPEG_INCLUDE_DIRS "${PACKETIA_SYSTEM_FFMPEG_INCLUDE_DIRS}" PARENT_SCOPE)
set(FFMPEG_LIBRARIES packetia_ffmpeg PARENT_SCOPE)

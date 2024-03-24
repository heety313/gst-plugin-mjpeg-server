cmake_minimum_required(VERSION 3.5)
project(mjpeg_http_sink)

#libgstreamer-plugins-base1.0-dev (required)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0)

include_directories(${GST_INCLUDE_DIRS})
link_directories(${GST_LIBRARY_DIRS})

add_library(mjpeghttpsink SHARED mjpeg_http_sink.c)
target_link_libraries(mjpeghttpsink ${GST_LIBRARIES})

install(TARGETS mjpeghttpsink LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/gstreamer-1.0)
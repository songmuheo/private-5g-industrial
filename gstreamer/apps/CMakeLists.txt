# gstreamer tree: video_sender / video_receiver against the distribution's GStreamer (pkg-config).
# System toolchain (g++), unlike the webrtc tree which needs libwebrtc's bundled clang + libc++.
cmake_minimum_required(VERSION 3.16)
project(p5g_gstreamer_apps CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.20 gstreamer-app-1.0 gstreamer-rtp-1.0 gstreamer-video-1.0 gstreamer-net-1.0 gio-2.0)
find_package(Threads REQUIRED)

set(P5G_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/common)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_executable(video_sender sender/video_sender.cc)
target_include_directories(video_sender PRIVATE ${P5G_INCLUDES})
target_link_libraries(video_sender PRIVATE PkgConfig::GST Threads::Threads)

add_executable(video_receiver receiver/video_receiver.cc)
target_include_directories(video_receiver PRIVATE ${P5G_INCLUDES})
target_link_libraries(video_receiver PRIVATE PkgConfig::GST Threads::Threads)

# Unit test of the RTCP validator (runs at build time from scripts/build_apps.sh).
add_executable(rtcp_valid_test tests/rtcp_valid_test.cc)
target_include_directories(rtcp_valid_test PRIVATE ${P5G_INCLUDES})
target_link_libraries(rtcp_valid_test PRIVATE PkgConfig::GST Threads::Threads)

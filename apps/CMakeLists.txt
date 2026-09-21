# WebRTC video sender / receiver, statically linked against a stock libwebrtc (M120).
#
# libwebrtc is built by Chromium's toolchain with its bundled clang + libc++ (use_custom_libcxx=true,
# ABI namespace __Cr). Anything that links libwebrtc.a must therefore be compiled with the same
# clang and the same libc++ headers, and must NOT also link the system libstdc++. build_apps.sh sets
# CMAKE_CXX_COMPILER to the bundled clang++ and provides:
#   -DLIBWEBRTC_SRC=<checkout>/src         (headers + out/Release/obj/libwebrtc.a)
#   -DLIBCXX_ARCHIVE_DIR=<dir>             (libc++.a / libc++abi.a archived from libwebrtc's objects)
cmake_minimum_required(VERSION 3.16)
project(p5g_webrtc_apps CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED LIBWEBRTC_SRC)
  message(FATAL_ERROR "LIBWEBRTC_SRC not set (use scripts/build/build_apps.sh)")
endif()
if(NOT DEFINED LIBCXX_ARCHIVE_DIR)
  message(FATAL_ERROR "LIBCXX_ARCHIVE_DIR not set (use scripts/build/build_apps.sh)")
endif()

set(WEBRTC_LIB "${LIBWEBRTC_SRC}/out/Release/obj/libwebrtc.a")
if(NOT EXISTS "${WEBRTC_LIB}")
  message(FATAL_ERROR "libwebrtc.a not found at ${WEBRTC_LIB} — run scripts/build/build_libwebrtc.sh")
endif()

# Bundled libc++ headers instead of the system C++ standard library. Raw CMAKE_CXX_FLAGS because
# add_compile_options() de-duplicates repeated -isystem tokens.
set(LIBCXX_FLAGS "-nostdinc++ \
-isystem ${LIBWEBRTC_SRC}/buildtools/third_party/libc++ \
-isystem ${LIBWEBRTC_SRC}/third_party/libc++/src/include \
-isystem ${LIBWEBRTC_SRC}/third_party/libc++abi/src/include")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LIBCXX_FLAGS} -Wall -Wno-unused-parameter")
add_link_options(-nostdlib++)   # single C++ runtime: the bundled static libc++ only

# WEBRTC_POSIX/LINUX: platform selection in libwebrtc headers.
# WEBRTC_USE_H264: header-only OpenH264 factory adapters report H264 only when this is defined
#                  (libwebrtc.a was built with rtc_use_h264=true, so the symbols exist).
# NDEBUG: match the Release build (RTC_DCHECK_IS_ON=0); otherwise headers reference debug-only symbols.
add_compile_definitions(WEBRTC_POSIX WEBRTC_LINUX WEBRTC_USE_H264 NDEBUG)

set(P5G_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/common
  ${LIBWEBRTC_SRC}
  ${LIBWEBRTC_SRC}/third_party/abseil-cpp
  ${LIBWEBRTC_SRC}/third_party/libyuv/include)

# libwebrtc.a references X11 (desktop capture module) even though we only use the video pipeline.
set(P5G_LINK
  -Wl,--start-group
    ${WEBRTC_LIB}
    ${LIBCXX_ARCHIVE_DIR}/libc++.a
    ${LIBCXX_ARCHIVE_DIR}/libc++abi.a
  -Wl,--end-group
  pthread dl m rt
  X11 Xext Xdamage Xfixes Xcomposite Xrandr Xtst)

add_executable(video_sender sender/video_sender.cc)
target_include_directories(video_sender PRIVATE ${P5G_INCLUDES})
target_link_libraries(video_sender PRIVATE ${P5G_LINK})

add_executable(video_receiver receiver/video_receiver.cc)
target_include_directories(video_receiver PRIVATE ${P5G_INCLUDES})
target_link_libraries(video_receiver PRIVATE ${P5G_LINK})

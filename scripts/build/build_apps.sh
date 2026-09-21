#!/usr/bin/env bash
# Build the WebRTC apps (video_sender / video_receiver) against libwebrtc.
#
# Toolchain: the clang++ bundled inside the libwebrtc checkout (the compiler that built
# libwebrtc.a) + libwebrtc's own libc++ (archived here from its loose object files). This gives a
# binary that depends only on glibc and X11 runtime libraries of the build host, so building on
# Ubuntu 22.04 produces binaries that run on any 22.04+ machine (the UE laptops).
#
# libwebrtc location: $P5G_LIBWEBRTC_DIR or third_party/libwebrtc (a symlink is fine).
# Output: build/apps/video_sender, build/apps/video_receiver
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LW="${P5G_LIBWEBRTC_DIR:-$ROOT/third_party/libwebrtc}"
SRC="$LW/src"
REL="$SRC/out/Release"
OUT="$ROOT/build/apps"
JOBS="${JOBS:-$(nproc)}"

if [ ! -f "$REL/obj/libwebrtc.a" ]; then
  echo "[build_apps] libwebrtc.a not found under $REL — run scripts/build/build_libwebrtc.sh" >&2
  exit 1
fi
CLANGXX="$SRC/third_party/llvm-build/Release+Asserts/bin/clang++"
if [ ! -x "$CLANGXX" ]; then
  echo "[build_apps] bundled clang++ not found at $CLANGXX" >&2
  exit 1
fi

mkdir -p "$OUT/libcxx"
# libwebrtc leaves libc++ / libc++abi as loose .o files; archive them once (or when they change).
if [ ! -f "$OUT/libcxx/libc++.a" ] || [ -n "$(find "$REL/obj/buildtools/third_party/libc++/libc++" -newer "$OUT/libcxx/libc++.a" -name '*.o' | head -1)" ]; then
  echo "[build_apps] archiving bundled libc++ / libc++abi"
  rm -f "$OUT/libcxx/libc++.a" "$OUT/libcxx/libc++abi.a"
  ar rcs "$OUT/libcxx/libc++.a"    "$REL"/obj/buildtools/third_party/libc++/libc++/*.o
  ar rcs "$OUT/libcxx/libc++abi.a" "$REL"/obj/buildtools/third_party/libc++abi/libc++abi/*.o
fi

# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY: CMake's compiler sanity check links a hello-world
# against the default C++ runtime (-lstdc++), which the bundled clang cannot find on a host without
# the matching libstdc++-dev. We never link libstdc++ (see apps/CMakeLists.txt), so skip that link.
cmake -S "$ROOT/apps" -B "$OUT/cmake" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_CXX_COMPILER="$CLANGXX" \
  -DLIBWEBRTC_SRC="$SRC" \
  -DLIBCXX_ARCHIVE_DIR="$OUT/libcxx"
ninja -C "$OUT/cmake" -j"$JOBS"
cp "$OUT/cmake/video_sender" "$OUT/cmake/video_receiver" "$OUT/"

# Record what was linked (libwebrtc commit) next to the binaries for provenance.
{
  echo "libwebrtc_commit=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "libwebrtc_dirty=$(if [ -n "$(git -C "$SRC" status --porcelain 2>/dev/null)" ]; then echo yes; else echo no; fi)"
  echo "built_at=$(date -Is)"
} > "$OUT/BUILD_INFO.txt"

echo "[build_apps] done:"
ls -la "$OUT/video_sender" "$OUT/video_receiver"
echo "[build_apps] runtime deps (must not list libstdc++/libc++):"
ldd "$OUT/video_sender" | grep -E "libstdc|libc\+\+" || echo "  ok: no dynamic C++ runtime"

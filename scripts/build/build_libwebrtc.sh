#!/usr/bin/env bash
# Fetch and build a STOCK libwebrtc (M120) into third_party/libwebrtc using Chromium's depot_tools,
# inside the p5g/libwebrtc-builder container (Ubuntu 24.04: depot_tools needs Python >= 3.11).
#
# Pinned by third_party/libwebrtc.lock (webrtc_commit, depot_tools_commit). First run: ~25 GB and
# 1-3 hours. Output: third_party/libwebrtc/src/out/Release/obj/libwebrtc.a (+ headers, bundled
# clang/libc++ used by scripts/build/build_apps.sh).
#
# If a pristine checkout already exists elsewhere (e.g. another project on the same machine), point
# P5G_LIBWEBRTC_DIR or a symlink third_party/libwebrtc at it instead of re-fetching; build_apps.sh
# records the commit and dirtiness of whatever it links.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LW="${P5G_LIBWEBRTC_DIR:-$ROOT/third_party/libwebrtc}"
LOCK="$ROOT/third_party/libwebrtc.lock"
IMG="p5g/libwebrtc-builder:24.04"

WEBRTC_COMMIT="$(grep '^webrtc_commit:' "$LOCK" | awk '{print $2}')"
DEPOT_COMMIT="$(grep '^depot_tools_commit:' "$LOCK" | awk '{print $2}')"
[ -n "$WEBRTC_COMMIT" ] || { echo "no webrtc_commit in $LOCK" >&2; exit 1; }
mkdir -p "$LW"

docker image inspect "$IMG" >/dev/null 2>&1 || docker build -f "$ROOT/docker/Dockerfile.libwebrtc-builder" -t "$IMG" "$ROOT"

docker run --rm --network host -v "$LW:/work" -e WEBRTC_COMMIT="$WEBRTC_COMMIT" -e DEPOT_COMMIT="$DEPOT_COMMIT" "$IMG" bash -euo pipefail -c '
  export PATH=/work/depot_tools:$PATH DEPOT_TOOLS_UPDATE=0
  if [ ! -d /work/depot_tools/.git ]; then
    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git /work/depot_tools
  fi
  [ -z "$DEPOT_COMMIT" ] || git -C /work/depot_tools checkout -q "$DEPOT_COMMIT"
  cd /work
  if [ ! -d src ]; then
    echo "[libwebrtc] fetch --nohooks webrtc (long)"; fetch --nohooks webrtc
  fi
  cd src
  git fetch --depth 1 origin "$WEBRTC_COMMIT"
  git checkout -q -B build "$WEBRTC_COMMIT"
  for attempt in 1 2 3; do gclient sync -D --force --reset --no-history --jobs 4 && break; sleep 30; done
  ./build/install-build-deps.sh --no-prompt --no-arm || true
  # Stock build: library only. use_custom_libcxx=true (bundled libc++) is what build_apps.sh expects.
  gn gen out/Release --args="is_debug=false rtc_include_tests=false rtc_build_examples=false rtc_build_tools=false rtc_use_h264=true ffmpeg_branding=\"Chrome\" use_rtti=true use_custom_libcxx=true treat_warnings_as_errors=false is_clang=true"
  ninja -C out/Release webrtc
  git status --porcelain | head -5
  ls -la out/Release/obj/libwebrtc.a
'
echo "[libwebrtc] done: $LW/src/out/Release/obj/libwebrtc.a"

#!/usr/bin/env bash
# Build the ffmpeg tree's video_sender / video_receiver -> ffmpeg/build/apps/, then run the depacketizer round-trip
# test on a 60-frame stream encoded on the fly (needs ffmpeg + libx264: make deps).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # ffmpeg/
OUT="$ROOT/build/apps"; mkdir -p "$OUT"
cmake -S "$ROOT/apps" -B "$OUT/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$OUT/cmake" -j"${JOBS:-$(nproc)}"
cp "$OUT/cmake/video_sender" "$OUT/cmake/video_receiver" "$OUT/cmake/depack_test" "$OUT/"
# round-trip test stream: 60 frames of a synthetic pattern, the same x264 settings prepare_video.sh uses
TS="$OUT/test_640x360.h264"
[ -f "$TS" ] || ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=30" -frames:v 60 -c:v libx264 -preset veryfast -tune zerolatency \
    -x264-params "keyint=30:min-keyint=30:scenecut=0:aud=1:repeat-headers=1" -b:v 600k -f h264 "$TS"
"$OUT/depack_test" "$TS" | tee "$OUT/depack_test.log" | grep -q "bad=0" || { echo "[build_apps] depacketizer test FAILED" >&2; exit 1; }
{
  echo "transport=ffmpeg"
  for l in libavformat libavcodec libavutil; do echo "$l=$(pkg-config --modversion $l)"; done
  echo "ffmpeg_cli=$(ffmpeg -version 2>/dev/null | head -1 | awk '{print $3}')"
  echo "libx264=$(dpkg-query -W -f='${Version}' libx264-163 2>/dev/null || echo unknown)"
  echo "built_at=$(date -Iseconds)"
} > "$OUT/BUILD_INFO.txt"
echo "[build_apps] $(cat "$OUT/depack_test.log")"; cat "$OUT/BUILD_INFO.txt"

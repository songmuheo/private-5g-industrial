#!/usr/bin/env bash
# Build the gstreamer tree's video_sender / video_receiver -> gstreamer/build/apps/.
# Needs (Ubuntu 22.04, `make deps`): libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev and the plugins
# gstreamer1.0-plugins-{base,good,bad,ugly} gstreamer1.0-libav (x264enc is in -ugly, h264parse in -bad,
# avdec_h264 in -libav). Records the GStreamer core/plugin versions in build/apps/BUILD_INFO.txt.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # gstreamer/
OUT="$ROOT/build/apps"
mkdir -p "$OUT"
JOBS="${JOBS:-$(nproc)}"
cmake -S "$ROOT/apps" -B "$OUT/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$OUT/cmake" -j"$JOBS"
cp "$OUT/cmake/video_sender" "$OUT/cmake/video_receiver" "$OUT/"
{
  echo "transport=gstreamer"
  echo "gstreamer_core=$(pkg-config --modversion gstreamer-1.0)"
  # (no early awk exit: with pipefail a SIGPIPE to gst-inspect would abort the script)
  for e in x264enc rtph264pay rtpbin rtph264depay avdec_h264 h264parse appsrc udpsink; do
    v="$(gst-inspect-1.0 "$e" 2>/dev/null | awk '/^  Version/{v=$2} END{print v}')"; echo "plugin_${e}=${v:-missing}"
  done
  echo "libx264=$(dpkg-query -W -f='${Version}' libx264-163 2>/dev/null || echo unknown)"
  echo "built_at=$(date -Iseconds)"
} > "$OUT/BUILD_INFO.txt"
ls -la "$OUT/video_sender" "$OUT/video_receiver"
cat "$OUT/BUILD_INFO.txt"

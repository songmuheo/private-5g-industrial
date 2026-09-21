#!/usr/bin/env bash
# Download a standard test sequence (Xiph / derf collection) and convert it to raw I420 for
# video_sender --yuv. Assets land in video/assets/ (git-ignored).
#   video/prepare_test_sequence.sh [name] [width height]
#   default: crowd_run 1280x720 (30 fps, 500 frames)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${1:-crowd_run}"; W="${2:-1280}"; H="${3:-720}"
OUT="$ROOT/video/assets"; mkdir -p "$OUT"
case "$NAME" in
  crowd_run)  URL="https://media.xiph.org/video/derf/y4m/crowd_run_1080p50.y4m";;
  park_joy)   URL="https://media.xiph.org/video/derf/y4m/park_joy_1080p50.y4m";;
  ducks_take_off) URL="https://media.xiph.org/video/derf/y4m/ducks_take_off_1080p50.y4m";;
  *) echo "unknown sequence $NAME (crowd_run|park_joy|ducks_take_off)" >&2; exit 1;;
esac
Y4M="$OUT/$(basename "$URL")"
YUV="$OUT/${NAME}_${W}x${H}_30fps_i420.yuv"
[ -f "$Y4M" ] || curl -L -o "$Y4M" "$URL"
command -v ffmpeg >/dev/null || { echo "ffmpeg required (sudo apt install ffmpeg)" >&2; exit 1; }
ffmpeg -y -i "$Y4M" -vf "scale=${W}:${H}" -r 30 -pix_fmt yuv420p -f rawvideo "$YUV"
FRAMES=$(( $(stat -c %s "$YUV") / (W * H * 3 / 2) ))
echo "$YUV  frames=$FRAMES"
echo "use: video_sender --yuv $YUV --width $W --height $H --fps 30"

#!/usr/bin/env bash
# Fetch a ready-made raw I420 asset from the host that already has it (the gNB PC) into video/assets/,
# instead of downloading and converting a test sequence. Derived from video/prepare_test_sequence.sh.
#   video/fetch_asset.sh                     default: the six Kendo multi-view files kendo_view{0..5}_1280x720_30.yuv
#                                            (415 MB each, 2.5 GB total) so any laptop can be any camera camK
#   video/fetch_asset.sh --cam K             only kendo_viewK (the laptop that runs --stream-id camK)
#   video/fetch_asset.sh [name ...]          explicit files, e.g. fade_walk_1280x720_30fps_300s_i420.yuv (12.4 GB, 300 s)
#   video/fetch_asset.sh --list              show what the source host has in its video/assets/
#   P5G_ASSET_HOST=user@host                 source host (default songmu@163.152.193.99 = gNB PC over the lab LAN;
#                                            use songmu@10.53.1.1 to pull over the 5G path, slower)
#   P5G_ASSET_DIR=/path/to/video/assets      source directory (default ~/private-5g-industrial/video/assets)
# Uses rsync when available (resumable, shows progress), otherwise scp. Verifies that the file size is a whole
# number of 1280x720 I420 frames (1,382,400 bytes each) and prints the frame count / duration at 30 fps.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${P5G_ASSET_HOST:-songmu@163.152.193.99}"
SRC_DIR="${P5G_ASSET_DIR:-private-5g-industrial/video/assets}"
OUT="$ROOT/video/assets"; mkdir -p "$OUT"
W=1280; H=720; FRAME_BYTES=$(( W * H * 3 / 2 ))

if [ "${1:-}" = "--list" ]; then
  ssh "$HOST" "ls -l $SRC_DIR/*.yuv 2>/dev/null || echo '(no .yuv on $HOST:$SRC_DIR)'"
  exit 0
fi
if [ "${1:-}" = "--cam" ]; then K="${2//[!0-9]/}"; [ -n "$K" ] || { echo "usage: --cam K" >&2; exit 1; }; set -- "kendo_view${K}_1280x720_30.yuv"; fi
[ $# -gt 0 ] || set -- kendo_view0_1280x720_30.yuv kendo_view1_1280x720_30.yuv kendo_view2_1280x720_30.yuv kendo_view3_1280x720_30.yuv kendo_view4_1280x720_30.yuv kendo_view5_1280x720_30.yuv

for NAME in "$@"; do
  DST="$OUT/$NAME"
  echo "[fetch_asset] $HOST:$SRC_DIR/$NAME -> $DST"
  if command -v rsync >/dev/null; then
    rsync -ah --partial --info=progress2 "$HOST:$SRC_DIR/$NAME" "$DST"
  else
    scp "$HOST:$SRC_DIR/$NAME" "$DST"
  fi
  SIZE=$(stat -c %s "$DST")
  if [ $(( SIZE % FRAME_BYTES )) -ne 0 ]; then
    echo "[fetch_asset] ERROR: $DST is $SIZE bytes, not a whole number of ${W}x${H} I420 frames (incomplete copy?)" >&2
    exit 1
  fi
  FRAMES=$(( SIZE / FRAME_BYTES ))
  echo "[fetch_asset] $DST  frames=$FRAMES  (= $(( FRAMES / 30 )) s at 30 fps)"
done
echo "[fetch_asset] run_sender.sh maps --stream-id camK to kendo_viewK automatically (then fade_walk > crowd_run > FourPeople); or pass --yuv explicitly."

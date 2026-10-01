#!/usr/bin/env bash
# One-shot preparation of a UE laptop for the ffmpeg tree (run ON the laptop, from the repo root or anywhere):
#   ffmpeg/scripts/prepare_client.sh <K> [gnb-pc-user@host] [--clean]
# K = the camera id this laptop plays (cam<K>). Steps: packages, build, this camera's pre-encoded rungs from the gNB
# PC over the sync LAN (the MOT17-03 ladder rotated to content origin P5G_IDR_STEP*K, default 3K: same frame numbers
# on every camera, IDRs staggered — encoded once there by prepare_video.sh --source mot17-03, so every host has
# bit-identical sources; P5G_ASSETS=all fetches every *.h264 instead), sha256 check against the manifest,
# sync-LAN/chrony setup if missing, and a go/no-go check. --clean first deletes every other video file in
# video/assets (*.h264, *.264, *.yuv, *.y4m, *.mp4, *.mkv; the gstreamer/webrtc YUVs come back with video/fetch_asset.sh). Re-runnable.
set -euo pipefail
K="${1:?camera id K (this laptop plays cam<K>)}"; shift
GNB=songmu@192.168.77.1; CLEAN=0
for a in "$@"; do case "$a" in --clean) CLEAN=1;; *) GNB="$a";; esac; done
ORIGIN=$(( ${P5G_IDR_STEP:-3} * K )); PATTERN="mot17-03-o${ORIGIN}_*.h264"; [ "${P5G_ASSETS:-}" = all ] && PATTERN='*.h264'
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; ROOT="$(cd "$TREE/.." && pwd)"; cd "$ROOT"
echo "[prepare_client] 1/5 packages"; sudo apt-get install -y --no-install-recommends cmake ninja-build build-essential pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev ffmpeg python3 chrony rsync >/dev/null
echo "[prepare_client] 2/5 build";    "$TREE/scripts/build_apps.sh" | tail -1
echo "[prepare_client] 3/5 pre-encoded rungs ($PATTERN) from $GNB (sync LAN)"; mkdir -p video/assets
if [ "$CLEAN" = 1 ]; then
  n=0; for f in video/assets/*.h264 video/assets/*.264 video/assets/*.yuv video/assets/*.y4m video/assets/*.mp4 video/assets/*.mkv; do [ -e "$f" ] || continue
    case "$(basename "$f")" in $PATTERN) ;; *) rm -f "$f"; n=$((n + 1));; esac; done
  echo "   --clean: removed $n other video file(s)"
fi
rsync -a --info=progress2 "$GNB:private-5g-industrial/video/assets/$PATTERN" "$GNB:private-5g-industrial/video/assets/h264_ladders.txt" video/assets/
bad=0; for f in video/assets/$PATTERN; do b="$(basename "$f")"
  w="$(grep "^file=$b " video/assets/h264_ladders.txt | sed -n 's/.* sha256=//p')"
  [ -n "$w" ] && [ "$w" = "$(sha256sum "$f" | cut -c1-64)" ] || { echo "   sha256 MISMATCH: $b" >&2; bad=1; }; done
[ "$bad" = 0 ] || { echo "[prepare_client] rung files do not match the gNB manifest" >&2; exit 1; }
echo "   $(ls video/assets/$PATTERN | wc -l) rung files, sha256 verified; video/assets now: $(du -sh video/assets | cut -f1)"
echo "[prepare_client] 4/5 clock-sync LAN"
if ! nmcli -t -f NAME con show 2>/dev/null | grep -qx p5g-sync; then scripts/setup/sync_lan_client.sh "$K"; else echo "   p5g-sync present"; fi
echo "[prepare_client] 5/5 checks"; scripts/setup/sync_check.sh 1 || echo "   chrony not yet within 1 ms (give it a minute)"
ip route show default | sed 's/^/   default route: /'
"$TREE/build/apps/video_sender" --help 2>&1 | head -1 | sed 's/^/   /'
echo "[prepare_client] done: this laptop plays cam$K; the gNB PC runs ffmpeg/run_experiment.sh"

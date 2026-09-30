#!/usr/bin/env bash
# One-shot preparation of a UE laptop for the ffmpeg tree (run ON the laptop, from the repo root or anywhere):
#   ffmpeg/scripts/prepare_client.sh <K> [gnb-pc-user@host]
# K = the camera id this laptop plays (cam<K>). Steps: packages, build, pre-encoded rungs from the gNB PC over
# the sync LAN (rsync of video/assets/*.h264 — encoded once there by ffmpeg/scripts/prepare_video.sh, so every
# host has bit-identical sources), sync-LAN/chrony setup if missing, and a go/no-go check. Re-runnable.
set -euo pipefail
K="${1:?camera id K (this laptop plays cam<K>)}"; GNB="${2:-songmu@192.168.77.1}"
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; ROOT="$(cd "$TREE/.." && pwd)"; cd "$ROOT"
echo "[prepare_client] 1/5 packages"; sudo apt-get install -y --no-install-recommends cmake ninja-build build-essential pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev ffmpeg python3 chrony rsync >/dev/null
echo "[prepare_client] 2/5 build";    "$TREE/scripts/build_apps.sh" | tail -1
echo "[prepare_client] 3/5 pre-encoded rungs from $GNB (sync LAN)"; mkdir -p video/assets
rsync -a --info=progress2 "$GNB:private-5g-industrial/video/assets/"'*.h264' "$GNB:private-5g-industrial/video/assets/h264_ladders.txt" video/assets/
ls video/assets/*.h264 | wc -l | xargs -I{} echo "   {} rung files"
echo "[prepare_client] 4/5 clock-sync LAN"
if ! nmcli -t -f NAME con show 2>/dev/null | grep -qx p5g-sync; then scripts/setup/sync_lan_client.sh "$K"; else echo "   p5g-sync present"; fi
echo "[prepare_client] 5/5 checks"; scripts/setup/sync_check.sh 1 || echo "   chrony not yet within 1 ms (give it a minute)"
ip route show default | sed 's/^/   default route: /'
"$TREE/build/apps/video_sender" --help 2>&1 | head -1 | sed 's/^/   /'
echo "[prepare_client] done: this laptop plays cam$K; the gNB PC runs ffmpeg/run_experiment.sh"

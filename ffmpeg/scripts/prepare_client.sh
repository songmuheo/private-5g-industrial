#!/usr/bin/env bash
# One-shot preparation of a UE laptop for the ffmpeg tree (run ON the laptop, from the repo root or anywhere):
#   ffmpeg/scripts/prepare_client.sh <K> [gnb-pc-user@host] [--clean]
# K = the camera id this laptop plays by default (cam<K>; sync-LAN address). Steps: packages, build, the pre-encoded
# rungs from the gNB PC over the sync LAN — every file any camera uses in the ffmpeg/experiments/*.json that use
# idr_origin (with run_experiment.sh --rotate any laptop may play any camera; ~450 MB) (rotated MOT17-03 ladders per
# profile and IDR origin, encoded once there by prepare_video.sh --source mot17-03, so every host has bit-identical
# sources; P5G_ASSETS=all fetches every *.h264 instead), sha256 check against the manifest,
# sync-LAN/chrony setup if missing, and a go/no-go check. --clean first deletes every other video file in
# video/assets (*.h264, *.264, *.yuv, *.y4m, *.mp4, *.mkv; the gstreamer/webrtc YUVs come back with video/fetch_asset.sh). Re-runnable.
set -euo pipefail
K="${1:?camera id K (this laptop plays cam<K>)}"; shift
GNB=songmu@192.168.77.1; CLEAN=0
for a in "$@"; do case "$a" in --clean) CLEAN=1;; *) GNB="$a";; esac; done
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; ROOT="$(cd "$TREE/.." && pwd)"; cd "$ROOT"
# the files = every rung file of every camera in the current-design scenarios (resolved exactly as run_experiment.sh does)
mapfile -t FILES < <(python3 - "$K" "$TREE/experiments" <<'PY'
import json, sys, glob
K, d = sys.argv[1], sys.argv[2]; out = set()
for f in sorted(glob.glob(f"{d}/*.json")):
    s = json.load(open(f))
    if s.get("hosts", {}).get("mode") == "local": continue
    if not any("idr_origin" in c for c in s.get("cams", {}).values()): continue   # current design only (aligned frames, per-camera IDR origin)
    for v in s["cams"].values():   # every camera: with run_experiment.sh --rotate any laptop may play any camera
        if not v.get("source"): continue
        src = v["source"]
        if "idr_origin" in v: name, rest = src.split("_", 1); src = f"{name}-o{int(v['idr_origin'])}_{rest}"
        for k in v.get("rungs") or [v.get("kbps", 2500)]: out.add(f"{src}_{int(k)}k.h264")
print("\n".join(sorted(out)))
PY
) || { echo "[prepare_client] could not read the scenarios in $TREE/experiments" >&2; exit 1; }
[ "${P5G_ASSETS:-}" = all ] && FILES=('*.h264')
[ "${#FILES[@]}" -gt 0 ] || { echo "no idr_origin scenario found in $TREE/experiments" >&2; exit 1; }
echo "[prepare_client] 1/5 packages"; sudo apt-get install -y --no-install-recommends cmake ninja-build build-essential pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev ffmpeg python3 chrony rsync >/dev/null
echo "[prepare_client] 2/5 build";    "$TREE/scripts/build_apps.sh" | tail -1
echo "[prepare_client] 3/5 pre-encoded rungs of all scenario cameras (${#FILES[@]} files) from $GNB (sync LAN)"; mkdir -p video/assets
if [ "$CLEAN" = 1 ]; then
  n=0; for f in video/assets/*.h264 video/assets/*.264 video/assets/*.yuv video/assets/*.y4m video/assets/*.mp4 video/assets/*.mkv; do [ -e "$f" ] || continue
    keep=0; for w in "${FILES[@]}"; do case "$(basename "$f")" in $w) keep=1;; esac; done; [ "$keep" = 1 ] || { rm -f "$f"; n=$((n + 1)); }; done
  echo "   --clean: removed $n other video file(s)"
fi
SRCS=(); for w in "${FILES[@]}"; do SRCS+=("$GNB:private-5g-industrial/video/assets/$w"); done
rsync -a --info=progress2 "${SRCS[@]}" "$GNB:private-5g-industrial/video/assets/h264_ladders.txt" video/assets/
bad=0; for w in "${FILES[@]}"; do for f in video/assets/$w; do b="$(basename "$f")"
  h="$(grep "^file=$b " video/assets/h264_ladders.txt | sed -n 's/.* sha256=//p')"
  [ -n "$h" ] && [ "$h" = "$(sha256sum "$f" | cut -c1-64)" ] || { echo "   sha256 MISMATCH: $b" >&2; bad=1; }; done; done
[ "$bad" = 0 ] || { echo "[prepare_client] rung files do not match the gNB manifest" >&2; exit 1; }
echo "   ${#FILES[@]} rung files, sha256 verified; video/assets now: $(du -sh video/assets | cut -f1)"
echo "[prepare_client] 4/5 clock-sync LAN"
if ! nmcli -t -f NAME con show 2>/dev/null | grep -qx p5g-sync; then scripts/setup/sync_lan_client.sh "$K"; else echo "   p5g-sync present"; fi
echo "[prepare_client] 5/5 checks"; scripts/setup/sync_check.sh 1 || echo "   chrony not yet within 1 ms (give it a minute)"
ip route show default | sed 's/^/   default route: /'
"$TREE/build/apps/video_sender" --help 2>&1 | head -1 | sed 's/^/   /'
echo "[prepare_client] done: this laptop plays cam$K; the gNB PC runs ffmpeg/run_experiment.sh"

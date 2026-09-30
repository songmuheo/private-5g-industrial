#!/usr/bin/env bash
# Prepare the pre-encoded sources of the ffmpeg tree (run once on the gNB PC; laptops fetch the results with
# scripts/prepare_client.sh). Output: video/assets/<name>_<WxH>_<fps>_<kbps>k.h264 — Annex B H.264, one AUD per
# access unit, SPS/PPS repeated on every IDR, fixed GOP, no scene-cut IDRs, CBR with HRD (so every rung's
# frame sizes are what the RAN-facing profile says), same frame count and IDR positions across the rungs of
# one source so the sender can switch rungs at an IDR.
#
#   ffmpeg/scripts/prepare_video.sh [--source mot17|kendo|all] [--rungs "500 1000 1500 2500 4000"] [--gop 60]
#
# Sources
#   mot17 : MOT17-02 (MOTChallenge MOT17.zip, 5.9 GB, cached in video/sources/mot17/; only the 600 frames of
#           MOT17-02-FRCNN/img1 are extracted): the sequence SMEC / ARMA / Pendulum stream (1080p30, 600 frames
#           = 20 s, looped). Rungs at 1280x720 (default) and one 1920x1080 8000k rung like SMEC's 8 Mbps.
#   kendo : the Nagoya Kendo views already in video/assets (kendo_view<K>_1280x720_30.yuv), one set per view,
#           for comparability with the gstreamer/webrtc runs (same content, same frame count 300).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # repo
ASSETS="$ROOT/video/assets"; SRC="$ROOT/video/sources"
SOURCE=all; RUNGS="500 1000 1500 2500 4000"; GOP=60; FPS=30
while [ $# -gt 0 ]; do case "$1" in --source) SOURCE="$2"; shift 2;; --rungs) RUNGS="$2"; shift 2;; --gop) GOP="$2"; shift 2;; *) echo "unknown arg $1" >&2; exit 1;; esac; done
mkdir -p "$ASSETS" "$SRC/mot17"
command -v ffmpeg >/dev/null || { echo "ffmpeg missing (make deps)" >&2; exit 1; }

# x264 settings shared by every rung: zerolatency (no B-frames, no lookahead), fixed GOP, AUD per frame,
# headers on every IDR, HRD-CBR with a one-frame VBV so the bytes per frame track the rung (bufsize = kbps/fps).
encode() {  # encode <input args...> -- <width> <height> <kbps> <out.h264>
  local in=(); while [ "$1" != "--" ]; do in+=("$1"); shift; done; shift
  local w="$1" h="$2" kbps="$3" out="$4"
  [ -f "$out" ] && { echo "  exists: $(basename "$out")"; return; }
  local bufk=$(( kbps / FPS * 2 ))   # 2 frame periods of VBV: CBR with room for the IDR (see docs/NOTES.md 2026-09-30 HRD note)
  ffmpeg -v error -y "${in[@]}" -vf "scale=${w}:${h}:flags=lanczos,format=yuv420p" -r "$FPS" \
    -c:v libx264 -preset medium -tune zerolatency -profile:v high -level 4.1 \
    -x264-params "keyint=${GOP}:min-keyint=${GOP}:scenecut=0:aud=1:repeat-headers=1:nal-hrd=cbr:force-cfr=1:bframes=0:rc-lookahead=0" \
    -b:v "${kbps}k" -minrate "${kbps}k" -maxrate "${kbps}k" -bufsize "${bufk}k" -f h264 "$out"
  echo "  $(basename "$out"): $(stat -c %s "$out" | awk '{printf "%.1f MB", $1/1e6}'), $(ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 "$out") frames"
}

if [ "$SOURCE" = mot17 ] || [ "$SOURCE" = all ]; then
  ZIP="$SRC/mot17/MOT17.zip"; IMG="$SRC/mot17/MOT17-02-FRCNN/img1"
  if [ ! -d "$IMG" ]; then
    [ -f "$ZIP" ] && [ "$(stat -c %s "$ZIP")" -ge 5860000000 ] || { echo "[prepare] downloading MOT17.zip (5.9 GB) ..."; curl -sSL -C - -o "$ZIP" https://motchallenge.net/data/MOT17.zip; }
    echo "[prepare] extracting MOT17-02-FRCNN/img1 (600 frames) ..."
    ( cd "$SRC/mot17" && unzip -q -o "$ZIP" "MOT17/train/MOT17-02-FRCNN/img1/*" && mv -f MOT17/train/MOT17-02-FRCNN . 2>/dev/null && rm -rf MOT17 )
  fi
  N=$(ls "$IMG"/*.jpg | wc -l); echo "[prepare] mot17-02: $N frames at $IMG"
  for k in $RUNGS; do encode -framerate "$FPS" -i "$IMG/%06d.jpg" -- 1280 720 "$k" "$ASSETS/mot17-02_1280x720_${FPS}_${k}k.h264"; done
  encode -framerate "$FPS" -i "$IMG/%06d.jpg" -- 1920 1080 8000 "$ASSETS/mot17-02_1920x1080_${FPS}_8000k.h264"   # SMEC's 1080p 8 Mbps
fi
if [ "$SOURCE" = kendo ] || [ "$SOURCE" = all ]; then
  for yuv in "$ASSETS"/kendo_view*_1280x720_30.yuv; do
    [ -f "$yuv" ] || continue
    v="$(basename "$yuv" | sed 's/_1280x720_30.yuv//')"
    for k in $RUNGS; do encode -f rawvideo -pix_fmt yuv420p -s 1280x720 -framerate "$FPS" -i "$yuv" -- 1280 720 "$k" "$ASSETS/${v}_1280x720_${FPS}_${k}k.h264"; done
  done
fi
# provenance next to the assets (which script, which settings produced them)
{ echo "generated_by=ffmpeg/scripts/prepare_video.sh"; echo "date=$(date -Iseconds)"; echo "gop=$GOP fps=$FPS rungs=\"$RUNGS\""
  echo "ffmpeg=$(ffmpeg -version | head -1 | awk '{print $3}') libx264=$(dpkg-query -W -f='${Version}' libx264-163 2>/dev/null)"; } > "$ASSETS/h264_ladders.txt"
echo "[prepare] done -> $ASSETS/*.h264 ($(ls "$ASSETS"/*.h264 | wc -l) files)"

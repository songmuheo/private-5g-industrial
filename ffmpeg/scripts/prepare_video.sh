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
# headers on every IDR, HRD-CBR with a two-frame VBV so the bytes per frame track the rung (bufsize = 2*kbps/fps).
#
# Each rung is encoded to a temporary file, checked (frame count = the source's, IDR positions = every GOP frames)
# and only then renamed into place; a settings line goes to the manifest. An existing rung is reused only when the
# manifest says it was made with the same settings and its frame count still matches (an interrupted or
# differently-configured encode is redone, never silently kept).
MANIFEST="$ASSETS/h264_ladders.txt"; NEW_MANIFEST="$ASSETS/.h264_ladders.new"; : > "$NEW_MANIFEST"
manifest_line() { echo "file=$(basename "$1") gop=$GOP fps=$FPS kbps=$2 size=$3x$4 frames=$5 idr=$6"; }
probe_frames() { ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 "$1" 2>/dev/null; }
# IDR positions: frame indices of key frames (AUD-delimited stream, so pict_type/key_frame per frame is exact)
probe_idr() { ffprobe -v error -select_streams v -show_entries frame=key_frame -of csv=p=0 "$1" 2>/dev/null | awk -F, 'NF{ if ($1==1) printf "%s%d", (n++?",":""), i; i++ } END{print ""}'; }   # NF: ffprobe emits an empty line for frame side data
encode() {  # encode <expected_frames> <input args...> -- <width> <height> <kbps> <out.h264>
  local want="$1"; shift
  local in=(); while [ "$1" != "--" ]; do in+=("$1"); shift; done; shift
  local w="$1" h="$2" kbps="$3" out="$4" tmp="$4.tmp"
  local expect_idr; expect_idr="$(seq 0 "$GOP" $((want - 1)) | paste -sd,)"
  local line; line="$(manifest_line "$out" "$kbps" "$w" "$h" "$want" "$expect_idr")"
  if [ -f "$out" ] && [ -f "$MANIFEST" ] && grep -qxF "$line" "$MANIFEST" && [ "$(probe_frames "$out")" = "$want" ]; then
    echo "  reuse: $(basename "$out") (same settings, $want frames)"; echo "$line" >> "$NEW_MANIFEST"; return
  fi
  [ -f "$out" ] && echo "  redo: $(basename "$out") (settings or frame count differ from the manifest)"
  local bufk=$(( kbps / FPS * 2 ))   # 2 frame periods of VBV: CBR with room for the IDR (see docs/NOTES.md 2026-09-30 HRD note)
  rm -f "$tmp"
  ffmpeg -v error -y "${in[@]}" -vf "scale=${w}:${h}:flags=lanczos,format=yuv420p" -r "$FPS" \
    -c:v libx264 -preset medium -tune zerolatency -profile:v high -level 4.1 \
    -x264-params "keyint=${GOP}:min-keyint=${GOP}:scenecut=0:aud=1:repeat-headers=1:nal-hrd=cbr:force-cfr=1:bframes=0:rc-lookahead=0" \
    -b:v "${kbps}k" -minrate "${kbps}k" -maxrate "${kbps}k" -bufsize "${bufk}k" -f h264 "$tmp"
  local got idr; got="$(probe_frames "$tmp")"; idr="$(probe_idr "$tmp")"
  [ "$got" = "$want" ] || { echo "  FAILED: $(basename "$out") has $got frames, expected $want (left as $tmp)" >&2; exit 1; }
  [ "$idr" = "$expect_idr" ] || { echo "  FAILED: $(basename "$out") IDR positions [$idr] != every $GOP frames (left as $tmp)" >&2; exit 1; }
  mv -f "$tmp" "$out"; echo "$line" >> "$NEW_MANIFEST"
  echo "  $(basename "$out"): $(stat -c %s "$out" | awk '{printf "%.1f MB", $1/1e6}'), $got frames, IDR every $GOP"
}

if [ "$SOURCE" = mot17 ] || [ "$SOURCE" = all ]; then
  ZIP="$SRC/mot17/MOT17.zip"; IMG="$SRC/mot17/MOT17-02-FRCNN/img1"
  if [ ! -d "$IMG" ]; then
    [ -f "$ZIP" ] && [ "$(stat -c %s "$ZIP")" -ge 5860000000 ] || { echo "[prepare] downloading MOT17.zip (5.9 GB) ..."; curl -sSL -C - -o "$ZIP" https://motchallenge.net/data/MOT17.zip; }
    echo "[prepare] extracting MOT17-02-FRCNN/img1 (600 frames) ..."
    ( cd "$SRC/mot17" && unzip -q -o "$ZIP" "MOT17/train/MOT17-02-FRCNN/img1/*" && mv -f MOT17/train/MOT17-02-FRCNN . 2>/dev/null && rm -rf MOT17 )
  fi
  N=$(ls "$IMG"/*.jpg | wc -l); echo "[prepare] mot17-02: $N frames at $IMG"
  [ "$N" -ge 600 ] || { echo "[prepare] MOT17-02 extraction incomplete ($N frames, expected 600); delete $SRC/mot17/MOT17-02-FRCNN and rerun" >&2; exit 1; }
  for k in $RUNGS; do encode "$N" -framerate "$FPS" -i "$IMG/%06d.jpg" -- 1280 720 "$k" "$ASSETS/mot17-02_1280x720_${FPS}_${k}k.h264"; done
  encode "$N" -framerate "$FPS" -i "$IMG/%06d.jpg" -- 1920 1080 8000 "$ASSETS/mot17-02_1920x1080_${FPS}_8000k.h264"   # SMEC's 1080p 8 Mbps
fi
if [ "$SOURCE" = kendo ] || [ "$SOURCE" = all ]; then
  for yuv in "$ASSETS"/kendo_view*_1280x720_30.yuv; do
    [ -f "$yuv" ] || continue
    v="$(basename "$yuv" | sed 's/_1280x720_30.yuv//')"
    N=$(( $(stat -c %s "$yuv") / (1280 * 720 * 3 / 2) ))   # I420 frames in the raw file
    for k in $RUNGS; do encode "$N" -f rawvideo -pix_fmt yuv420p -s 1280x720 -framerate "$FPS" -i "$yuv" -- 1280 720 "$k" "$ASSETS/${v}_1280x720_${FPS}_${k}k.h264"; done
  done
fi
# manifest next to the assets: provenance + one validated line per rung (lines of rungs not touched by this
# invocation — e.g. --source kendo after mot17 — are carried over so the manifest stays complete)
{ echo "generated_by=ffmpeg/scripts/prepare_video.sh"; echo "date=$(date -Iseconds)"; echo "last_args=source=$SOURCE gop=$GOP fps=$FPS rungs=\"$RUNGS\""
  echo "ffmpeg=$(ffmpeg -version | head -1 | awk '{print $3}') libx264=$(dpkg-query -W -f='${Version}' libx264-163 2>/dev/null)"
  cat "$NEW_MANIFEST"
  if [ -f "$MANIFEST" ]; then
    while read -r l; do f="${l#file=}"; f="${f%% *}"; grep -q "^file=$f " "$NEW_MANIFEST" || { [ -f "$ASSETS/$f" ] && echo "$l"; } || true; done < <(grep '^file=' "$MANIFEST" || true)
  fi
} > "$MANIFEST.new"
mv -f "$MANIFEST.new" "$MANIFEST"; rm -f "$NEW_MANIFEST"
echo "[prepare] done -> $ASSETS/*.h264 ($(ls "$ASSETS"/*.h264 | wc -l) files)"

#!/usr/bin/env bash
# Get the fade_walk clip from the web and convert it to the raw I420 asset the senders use.
# Derived from video/prepare_test_sequence.sh (which downloads xiph test sequences); this one uses yt-dlp.
#
#   video/prepare_fade_walk.sh [seconds]            default 300 s -> video/assets/fade_walk-web_1280x720_30fps_300s_i420.yuv
#   P5G_FADE_WALK_FMT=248|313|137|136                yt-dlp format id (default 248 = 1080p VP9 webm, ~270 MB;
#                                                    313 = 2160p VP9, 1.5 GB, what ran-gcc's masters were made from)
#
# Source: "Just An Old School Walk N Talk", YouTube id C4Z4TWZUqns, Life In Motion Diaries, 742 s, 24 fps,
# Creative Commons Attribution (reuse allowed). The ran-gcc project made its fade_walk_{720,1080}p.mp4 masters
# from this clip (4K webm -> ffmpeg -> 30 fps H.264). This script goes straight to raw I420: scale to 1280x720,
# 24 -> 30 fps (ffmpeg -r 30 duplicates every 4th frame, as the masters do), first <seconds> seconds.
#
# NOTE: the result is equivalent to, but not bit-identical with, the copy that video/fetch_asset.sh pulls from the
# gNB PC (different decode/scale chain), so it gets a distinct name (fade_walk-web_...). Use ONE of the two ways on
# all laptops of an experiment so every sender feeds identical frames. Needs yt-dlp (pip install -U yt-dlp) and ffmpeg.
#
# STATUS 2026-09-28: YouTube currently answers HTTP 403 after ~20 MB for every yt-dlp client that has no GVS
# "PO Token" (tested: default/android_vr, tv, web_safari, mweb, android, ios; chunked and unchunked; ffmpeg
# downloader). Fixes are (a) a PO-token provider plugin, e.g. bgutil-ytdlp-pot-provider
# (https://github.com/yt-dlp/yt-dlp/wiki/PO-Token-Guide), then P5G_YTDLP_ARGS='--extractor-args youtube:...';
# or (b) simply video/fetch_asset.sh (copy from the gNB PC over the LAN), which is the reliable path today.
#   P5G_YTDLP_ARGS="..."   extra yt-dlp arguments (PO token, cookies, plugin options), appended as-is.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECS="${1:-300}"; W=1280; H=720; FPS=30
VID="C4Z4TWZUqns"; URL="https://www.youtube.com/watch?v=$VID"
FMT="${P5G_FADE_WALK_FMT:-248}"
SRC_DIR="$ROOT/video/sources"; OUT="$ROOT/video/assets"; mkdir -p "$SRC_DIR" "$OUT"
YUV="$OUT/fade_walk-web_${W}x${H}_${FPS}fps_${SECS}s_i420.yuv"

for tool in ffmpeg yt-dlp; do command -v "$tool" >/dev/null || { echo "$tool required (ffmpeg: sudo apt install ffmpeg; yt-dlp: pip install -U yt-dlp)" >&2; exit 1; }; done
# yt-dlp needs a JavaScript runtime for YouTube (since 2025; without one the download fails with HTTP 403).
# deno is its default; node (e.g. from nvm) works too.
JSRT=()
if command -v deno >/dev/null; then JSRT=(--js-runtimes deno)
else
  export NVM_DIR="${NVM_DIR:-$HOME/.nvm}"; [ -s "$NVM_DIR/nvm.sh" ] && . "$NVM_DIR/nvm.sh" >/dev/null 2>&1 || true
  if command -v node >/dev/null; then JSRT=(--js-runtimes "node:$(command -v node)")
  else echo "[fade_walk] WARNING: no deno/node found; YouTube may answer 403. Install deno: curl -fsSL https://deno.land/install.sh | sh" >&2; fi
fi

# 1. download (video only, no audio) into a temp name; renamed only when complete, so a re-run never reuses a
#    truncated file. Kept in video/sources/ so a re-run only reconverts.
SRC="$(ls "$SRC_DIR"/fade_walk_src_f${FMT}.* 2>/dev/null | grep -v '\.part$' | head -1 || true)"
if [ -n "$SRC" ] && ! ffprobe -v error -show_entries format=duration -of csv=p=0 "$SRC" 2>/dev/null | awk -v s="$SECS" '{exit !($1>=s)}'; then
  echo "[fade_walk] existing $SRC is shorter than $SECS s or unreadable -> re-downloading"; rm -f "$SRC"; SRC=""
fi
if [ -z "$SRC" ]; then
  echo "[fade_walk] downloading $URL format $FMT ..."
  # shellcheck disable=SC2086
  yt-dlp "${JSRT[@]}" --no-update ${P5G_YTDLP_ARGS:-} -f "$FMT/bv*[height>=720][ext=webm]/bv*[height>=720]/bv*" \
         -o "$SRC_DIR/fade_walk_src_f${FMT}.%(ext)s" "$URL" || {
    echo "[fade_walk] download failed. If it stopped with HTTP 403 after ~20 MB, YouTube wants a PO token: see the header" >&2
    echo "[fade_walk] of this script, or copy the asset from the gNB PC instead: video/fetch_asset.sh" >&2; exit 1; }
  SRC="$(ls "$SRC_DIR"/fade_walk_src_f${FMT}.* | grep -v '\.part$' | head -1)"
fi
echo "[fade_walk] source: $SRC ($(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,r_frame_rate:format=duration -of csv=p=0 "$SRC" | tr '\n' ' '))"

# 2. convert: first SECS seconds, scale, 30 fps, raw I420
ffmpeg -y -loglevel error -i "$SRC" -t "$SECS" -vf "scale=${W}:${H}" -r "$FPS" -pix_fmt yuv420p -f rawvideo "$YUV"
FRAMES=$(( $(stat -c %s "$YUV") / (W * H * 3 / 2) ))
echo "$YUV  frames=$FRAMES  (= $(( FRAMES / FPS )) s at $FPS fps)"
echo "use: run_sender.sh picks it automatically after fade_walk (fetch_asset copy); or video_sender --yuv $YUV --width $W --height $H --fps $FPS"

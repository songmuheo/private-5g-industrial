#!/usr/bin/env bash
# Get the Nagoya University "Kendo" multi-view test sequence (MPEG 3DV/FTV, 7 synchronized cameras of one
# scene) and convert the views to the raw I420 assets the senders use, one view per camera/UE.
# Derived from video/prepare_test_sequence.sh; the transcode is byte-identical to ran-mec's
# work/assets/kendo_view{0..5}_1280x720_30.yuv (same ffmpeg chain, sha256 checked), so assets made here and
# copies made with video/fetch_asset.sh are interchangeable.
#
#   video/prepare_kendo.sh [views]      default "0 1 2 3 4 5"; e.g. "0 1 2" for three cameras
#   P5G_KENDO_FRAMES=300                 frames per view (native 400 = 13.3 s; 300 = 10 s, matches ran-mec)
#
# Source: https://www.fujii.nuee.nagoya-u.ac.jp/multiview-data/mpeg2/Kendo/KendoImages(YUV).zip (1.67 GB,
# sha256 bccc54ad...9b48cb below), 7 views, raw I420 1024x768 30 fps 400 frames each. Academic /
# non-commercial; cite M. Tanimoto, T. Fujii, K. Suzuki et al., MPEG 3DV/FTV test sequences, Nagoya Univ.
# Transcode per view: centre-crop 4:3 -> 16:9 (1024x576), scale 1280x720, first N frames (identical crop on all
# views keeps the multi-view geometry consistent). Output: video/assets/kendo_view<V>_1280x720_30.yuv.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VIEWS="${1:-0 1 2 3 4 5}"; FRAMES="${P5G_KENDO_FRAMES:-300}"
URL="https://www.fujii.nuee.nagoya-u.ac.jp/multiview-data/mpeg2/Kendo/KendoImages(YUV).zip"
ZIP_SHA="bccc54ad795958b25ada5f6c08b8277c7e1972c4d636d0a42238fc988309b7cf"
# sha256 of the 300-frame outputs as produced in ran-mec (2026-08-16); only checked when FRAMES=300
declare -A VIEW_SHA=(
  [0]=888686b343bf651b652ad63e644658cc2b54c3b6bf105351c629345c2ed4e04e [1]=b693567c716d0b4d1127a67f3bd16c16be3e89b3eb34609b6d649a6b58402b7f
  [2]=f6ac14e174cb5c73fb45877fefd6b45a3da8c78b8a29a1954252b7c9f26dcee8 [3]=7108e639543f34661d6ef6667347529075c5fdb35cd5f9fcc7e08ae6333f7861
  [4]=c205a5962b25bd0a965060c8855eb64507a11f935100bd979f1f01924a2923ad [5]=3e55cac0c4eaeb445153413464a116b4085fa86e3b995f6cc8912abd539b48cb)
SRC_DIR="$ROOT/video/sources/kendo"; OUT="$ROOT/video/assets"; mkdir -p "$SRC_DIR" "$OUT"
ZIP="$SRC_DIR/KendoImages_YUV.zip"
for tool in curl unzip ffmpeg sha256sum; do command -v "$tool" >/dev/null || { echo "$tool required (sudo apt install $tool)" >&2; exit 1; }; done

# 1. download (resumable) + checksum
if [ ! -f "$ZIP" ] || ! echo "$ZIP_SHA  $ZIP" | sha256sum -c --quiet 2>/dev/null; then
  echo "[kendo] downloading $URL (1.67 GB, resumable) ..."
  curl -# -L -C - --retry 5 --retry-delay 5 -o "$ZIP" "$URL"     # measured 2026-09-28: 1.67 GB in ~3 min (9 MB/s) from the lab
  echo "$ZIP_SHA  $ZIP" | sha256sum -c || { echo "[kendo] checksum mismatch, delete $ZIP and retry" >&2; exit 1; }
fi

# 2. unzip the raw views (names inside the zip are kendoN.yuv-style; take them in sorted order = camera order)
mapfile -t RAW < <(unzip -Z1 "$ZIP" | grep -i '\.yuv$' | sort -V)
[ "${#RAW[@]}" -ge 6 ] || { echo "[kendo] unexpected zip contents:"; unzip -Z1 "$ZIP"; exit 1; }
for V in $VIEWS; do
  raw="${RAW[$V]}"; rawpath="$SRC_DIR/$(basename "$raw")"
  [ -f "$rawpath" ] || unzip -o -q "$ZIP" "$raw" -d "$SRC_DIR"
  [ -f "$rawpath" ] || rawpath="$(find "$SRC_DIR" -name "$(basename "$raw")" | head -1)"
  YUV="$OUT/kendo_view${V}_1280x720_30.yuv"
  echo "[kendo] view $V: $raw -> $YUV ($FRAMES frames)"
  ffmpeg -y -loglevel error -f rawvideo -pix_fmt yuv420p -s 1024x768 -r 30 -i "$rawpath" \
         -vf "crop=1024:576,scale=1280:720,setsar=1" -frames:v "$FRAMES" -pix_fmt yuv420p -f rawvideo "$YUV"
  n=$(( $(stat -c %s "$YUV") / (1280*720*3/2) )); echo "[kendo]   frames=$n (= $(( n / 30 )) s)"
  if [ "$FRAMES" = 300 ] && [ -n "${VIEW_SHA[$V]:-}" ]; then
    echo "${VIEW_SHA[$V]}  $YUV" | sha256sum -c --quiet && echo "[kendo]   sha256 OK (identical to ran-mec asset)" || echo "[kendo]   WARNING: sha256 differs from ran-mec's asset (ffmpeg version?)" >&2
  fi
done
echo "[kendo] done. run_sender.sh maps --stream-id camK to kendo_viewK automatically; or pass --yuv explicitly."

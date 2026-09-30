#!/usr/bin/env bash
# Fetch the upstream GStreamer examples our code follows into gstreamer/gstreamer-src (reference only,
# ignored by git, never built). Sparse, blobless, one tag (gstreamer/gstreamer.lock).
#   gstreamer/scripts/fetch_gst_examples.sh
set -euo pipefail
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$TREE/gstreamer.lock"; DST="$TREE/gstreamer-src"
TAG="$(sed -n 's/^gstreamer_tag: *//p' "$LOCK")"; URL="$(sed -n 's/^gstreamer_repo: *//p' "$LOCK")"
mapfile -t PATHS < <(sed -n 's/^  - //p' "$LOCK")
if [ ! -d "$DST/.git" ]; then
  git clone --quiet --depth 1 --branch "$TAG" --filter=blob:none --sparse "$URL" "$DST"
fi
git -C "$DST" sparse-checkout set "${PATHS[@]}"
echo "[gst-examples] $DST @ $(git -C "$DST" describe --tags --always)"
for p in "${PATHS[@]}"; do printf '  %-60s %s files\n' "$p" "$(find "$DST/$p" -type f | wc -l)"; done

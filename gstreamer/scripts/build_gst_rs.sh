#!/usr/bin/env bash
# Build the GStreamer Rust rtp plugin (rsrtp: rtpgccbwe, the Google Congestion Control bandwidth estimator)
# from its crates.io release, pinned in gstreamer/gstreamer.lock, into gstreamer/build/gst-plugins-rs/.
#   gstreamer/scripts/build_gst_rs.sh          -> gstreamer/build/gst-plugins-rs/libgstrsrtp.so
# Needs: rustc/cargo (Ubuntu 22.04: apt install rustc cargo; 1.75 >= the crate's MSRV 1.71), libgstreamer1.0-dev.
# The run scripts export GST_PLUGIN_PATH=gstreamer/build/gst-plugins-rs so gst-inspect / the apps find it.
# The .so links only GStreamer/GLib: the one built on the gNB PC can be copied to the laptops (same packages).
set -euo pipefail
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$TREE/gstreamer.lock"
CRATE="$(sed -n 's/^gst_plugin_rtp_crate: *//p' "$LOCK")"; VER="$(sed -n 's/^gst_plugin_rtp_version: *//p' "$LOCK")"
SRC="$TREE/build/gst-plugins-rs/src/$CRATE-$VER"; OUT="$TREE/build/gst-plugins-rs"
mkdir -p "$OUT/src"
if [ ! -f "$SRC/Cargo.toml" ]; then
  curl -sSL "https://crates.io/api/v1/crates/$CRATE/$VER/download" | tar -xz -C "$OUT/src"
fi
( cd "$SRC" && cargo build --release --locked 2>/dev/null || cargo build --release )
cp "$SRC/target/release/libgstrsrtp.so" "$OUT/"
GST_PLUGIN_PATH="$OUT" gst-inspect-1.0 rtpgccbwe | grep -E "^  (Version|Long-name)|estimated-bitrate|min-bitrate|max-bitrate" | head -8
echo "rsrtp=$VER" > "$OUT/BUILD_INFO.txt"; echo "[build_gst_rs] $OUT/libgstrsrtp.so"

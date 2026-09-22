#!/usr/bin/env bash
# Sender PC (laptop tethered to the Pixel): run video_sender against the receiver host.
#
#   ./run_sender.sh <receiver-host> [extra video_sender args...]
#
#   receiver-host : where ./run_receiver.sh runs (relay on TCP 8765). On the testbed that is the gNB
#                   PC's core-bridge address 10.53.1.1, reachable from the UE through the UPF.
#   extra args    : any video_sender flag; later flags override the defaults below, e.g.
#                   --width 1920 --height 1080 --fps 30 --yuv file.yuv --duration 120 --degradation stock
#
# Defaults for a fixed-resolution experiment: H264 1280x720@30, --degradation maintain_resolution,
# --start-bitrate-kbps auto (derived from the resolution, logged), 60 s. Traces -> results/<ts>-sender-<stream>/app
# (<stream>-tx-*.csv, -tx-stats.jsonl, sender-<stream>.log). Copy that app/ next to the receiver's for `make verify`.
#
# Binary: $P5G_SENDER_BIN, else build/apps/video_sender under this repo, else ./video_sender next to
# this script (copied from the build host).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
HOST="${1:?usage: run_sender.sh <receiver-host> [video_sender args...]}"; shift
BIN="${P5G_SENDER_BIN:-}"
[ -n "$BIN" ] || for c in build/apps/video_sender ./video_sender; do [ -x "$c" ] && { BIN="$c"; break; }; done
[ -n "$BIN" ] || { echo "video_sender binary not found (P5G_SENDER_BIN, build/apps/, or next to this script)" >&2; exit 1; }
# Run directory and log carry the stream id, so several senders (or repeated starts within one second)
# never share a directory or a log file.
STREAM=cam0; prev=""; for a in "$@"; do [ "$prev" = "--stream-id" ] && STREAM="$a"; prev="$a"; done
RD="results/$(date +%Y%m%d-%H%M%S)-sender-$STREAM"
mkdir -p "$RD/app"
echo "[sender] route to $HOST: $(ip route get "$HOST" 2>/dev/null | head -1 || echo '(unknown)')"
echo "[sender] run dir: $RD   binary: $BIN"
"$BIN" --signaling-host "$HOST" --signaling-port 8765 --trace-dir "$RD/app" \
       --codec H264 --width 1280 --height 720 --fps 30 \
       --degradation maintain_resolution --start-bitrate-kbps auto --duration 60 "$@" 2>&1 | (trap '' INT; exec tee "$RD/app/sender-$STREAM.log")
echo "[sender] done. traces: $RD/app"

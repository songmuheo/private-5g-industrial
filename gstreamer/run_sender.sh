#!/usr/bin/env bash
# UE laptop (USB-tethered to a Pixel): run the gstreamer video_sender against the receiver host.
#
#   ./run_sender.sh <receiver-host> [--start-at T] [video_sender args...]
#
#   receiver-host : where ./run_receiver.sh runs (control channel on TCP 8765; RTP goes to the same host unless
#                   the receiver advertises another). On the testbed that is the gNB PC's
#                   core-bridge address 10.53.1.1, reachable from the UE through the UPF.
#   --start-at T  : wait until wall-clock time T before starting (HH:MM:SS today, or a UNIX epoch with fraction).
#                   The same T on every laptop = synchronised start; needs the clock sync below.
#   other args    : any video_sender flag; later flags override the defaults, e.g.
#                   --width 640 --height 360 --fps 15 --bitrate-kbps 800 --gop 30 --yuv file.yuv --duration 60
#
# Defaults: H264 1280x720@30, --bitrate-kbps 2500 (x264 target; fixed profile, no adaptation), gop 2 s,
# 300 s. Source: video/assets/kendo_view<K>_1280x720_30.yuv for
# --stream-id cam<K> (Nagoya "Kendo" multi-view, one real camera angle per UE; video/fetch_asset.sh), else
# video/assets/fade_walk_1280x720_30fps_300s_i420.yuv, else any .yuv there, else the synthetic pattern
# (loud warning). Traces -> results/<ts>-sender-<stream>/app.
#
# Clock sync (P5G_SYNC=auto): if this laptop is not yet on the sync LAN (no NetworkManager connection
# "p5g-sync"), scripts/setup/sync_lan_client.sh <K> is run once (wired port auto-detected, 192.168.77.1K,
# chrony -> gNB PC; asks for sudo). Every run then checks chrony: RMS offset must be <= P5G_SYNC_MAX_MS (1).
# NO-GO is a warning, but aborts with --start-at. P5G_SYNC=off skips all of it.
#
# Binary: $P5G_SENDER_BIN, else gstreamer/build/apps/video_sender, else ./video_sender next to this script.
#
# Layout: this script lives in gstreamer/ (the active transport tree); results/, video/assets and
# scripts/setup are shared at the repo root, so the working directory is the repo root.
set -euo pipefail
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TREE/.." && pwd)"
cd "$ROOT"
HOST="${1:?usage: run_sender.sh <receiver-host> [--start-at T] [video_sender args...]}"; shift
BIN="${P5G_SENDER_BIN:-}"
[ -n "$BIN" ] || for c in "$TREE/build/apps/video_sender" "$TREE/video_sender"; do [ -x "$c" ] && { BIN="$c"; break; }; done
[ -n "$BIN" ] || { echo "video_sender binary not found (P5G_SENDER_BIN, gstreamer/build/apps/, or next to this script)" >&2; exit 1; }

# script-only options; everything else goes to video_sender
START_AT=""; ARGS=(); EXPLICIT_YUV=0
while [ $# -gt 0 ]; do
  case "$1" in
    --start-at) START_AT="$2"; shift 2;;
    --start-at=*) START_AT="${1#--start-at=}"; shift;;
    --yuv|--yuv=*) EXPLICIT_YUV=1; ARGS+=("$1"); shift;;
    *) ARGS+=("$1"); shift;;
  esac
done
set -- "${ARGS[@]}"
STREAM=cam0; prev=""; for a in "$@"; do [ "$prev" = "--stream-id" ] && STREAM="$a"; prev="$a"; done
K="${STREAM//[!0-9]/}"

# clock sync: one-time setup, then a go/no-go check
P5G_SYNC="${P5G_SYNC:-auto}"; P5G_SYNC_MAX_MS="${P5G_SYNC_MAX_MS:-1}"; SYNC_OK=1
if [ "$P5G_SYNC" != "off" ]; then
  if ! nmcli -t -f NAME con show 2>/dev/null | grep -qx p5g-sync; then
    if [ -n "$K" ]; then echo "[sender] sync LAN not configured -> scripts/setup/sync_lan_client.sh $K"; scripts/setup/sync_lan_client.sh "$K" || echo "[sender] WARNING: sync-LAN setup failed; continuing without clock sync" >&2
    else echo "[sender] WARNING: no camera id in --stream-id $STREAM; skipping clock-sync setup" >&2; fi
  fi
  if scripts/setup/sync_check.sh "$P5G_SYNC_MAX_MS" 2>/dev/null | sed 's/^/[sync] /'; then :; else SYNC_OK=0; fi
  if [ "$SYNC_OK" = 0 ]; then
    if [ -n "$START_AT" ]; then echo "[sender] clock sync NO-GO and --start-at given -> aborting (wait for chrony, or P5G_SYNC=off)" >&2; exit 1
    else echo "[sender] WARNING: clock sync NO-GO; cross-host timestamps of this run are not trustworthy" >&2; fi
  fi
fi

# run directory carries the stream id, so several senders never share a directory or a log
RD="results/$(date +%Y%m%d-%H%M%S)-sender-$STREAM"
mkdir -p "$RD/app"
echo "[sender] route to $HOST: $(ip route get "$HOST" 2>/dev/null | head -1 || echo '(unknown)')"
echo "[sender] run dir: $RD   binary: $BIN"
{ date -u +"start_utc=%Y-%m-%dT%H:%M:%S.%NZ"; chronyc tracking 2>/dev/null || echo "chrony: not available"; } > "$RD/app/clock-$STREAM.txt"

# video source
YUV_ARGS=()
if [ "$EXPLICIT_YUV" = 0 ]; then
  SRC=""
  [ -n "$K" ] && [ -f "video/assets/kendo_view${K}_1280x720_30.yuv" ] && SRC="video/assets/kendo_view${K}_1280x720_30.yuv"
  [ -n "$SRC" ] || { [ -f video/assets/fade_walk_1280x720_30fps_300s_i420.yuv ] && SRC=video/assets/fade_walk_1280x720_30fps_300s_i420.yuv; }
  [ -n "$SRC" ] || SRC="$(ls video/assets/*.yuv 2>/dev/null | head -1 || true)"
  if [ -n "$SRC" ]; then YUV_ARGS=(--yuv "$SRC"); echo "[sender] source: $SRC" | tee -a "$RD/app/clock-$STREAM.txt"
  else echo "[sender] WARNING: no .yuv in video/assets -> synthetic pattern (run video/fetch_asset.sh --cam $K)" | tee -a "$RD/app/clock-$STREAM.txt" >&2; fi
fi

# synchronised start
if [ -n "$START_AT" ]; then
  if [[ "$START_AT" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then TARGET="$START_AT"; else TARGET="$(date -d "today $START_AT" +%s.%N)" || { echo "bad --start-at $START_AT" >&2; exit 1; }; fi
  WAIT="$(awk -v t="$TARGET" -v n="$(date +%s.%N)" 'BEGIN{printf "%.3f", t-n}')"
  if [ "$(awk -v w="$WAIT" 'BEGIN{print (w<0)?1:0}')" = 1 ]; then echo "[sender] --start-at $START_AT is in the past (by ${WAIT#-} s); starting now" >&2
  else echo "[sender] waiting ${WAIT} s until $(date -d "@$TARGET" +%H:%M:%S.%N) ..."; sleep "$WAIT"; fi
  echo "[sender] started at $(date +%H:%M:%S.%N) (target $START_AT)" | tee -a "$RD/app/clock-$STREAM.txt"
fi

"$BIN" --control-host "$HOST" --control-port 8765 --trace-dir "$RD/app" \
       --width 1280 --height 720 --fps 30 "${YUV_ARGS[@]}" \
       --bitrate-kbps 2500 --duration 300 "$@" 2>&1 | (trap '' INT; exec tee "$RD/app/sender-$STREAM.log")
echo "[sender] done. traces: $RD/app"

#!/usr/bin/env bash
# UE laptop (USB-tethered to a Pixel): run the ffmpeg-tree video_sender (pre-encoded H.264 -> RTP/UDP) against the receiver host.
#
#   ./run_sender.sh <receiver-host> [--start-at T] [video_sender args...]
#
#   receiver-host : where ./run_receiver.sh runs (control channel on TCP $P5G_CONTROL_PORT, default 8765; RTP goes to the same host unless
#                   the receiver advertises another). On the testbed that is the gNB PC's
#                   core-bridge address 10.53.1.1, reachable from the UE through the UPF.
#   --start-at T  : wall-clock epoch of the capture grid (HH:MM:SS today, or a UNIX epoch with fraction): the
#                   sender connects, negotiates and then captures at T + k/fps (video_sender --start-at-epoch),
#                   so the same T on every laptop = frames and IDRs in phase across cameras. Needs the clock
#                   sync below. (The wait happens inside the app, after preparation, not in this shell.)
#   other args    : any video_sender flag; later flags override the defaults, e.g.
#                   --fps 30 --bitrate-kbps 1000 --duration 60 --source video/assets/x_2500k.h264@2500,...
#
# Defaults: fps 30, 300 s, --bitrate-kbps 2500 (the rung to start with). Source: unless --source is given, the
# pre-encoded rungs of the camera's content (ffmpeg/scripts/prepare_video.sh): P5G_SOURCE (a name prefix such as
# mot17-02_1280x720_30 or kendo_view<K>_1280x720_30, default kendo_view<K>... if present else mot17-02...), with
# every rung file <prefix>_<kbps>k.h264 found in video/assets (P5G_RUNGS="500 1000 ..." restricts). No encoder
# runs on the laptop. Traces -> results/<ts>-sender-<stream>/app (P5G_RUN_ID=<id> -> results/<id>-sender-<stream>,
# which is how run_experiment.sh knows exactly which directory belongs to its launch).
#
# Clock sync (P5G_SYNC=auto): if this laptop is not yet on the sync LAN (no NetworkManager connection
# "p5g-sync"), scripts/setup/sync_lan_client.sh <K> is run once (wired port auto-detected, 192.168.77.1K,
# chrony -> gNB PC; asks for sudo). Every run then checks chrony: RMS offset must be <= P5G_SYNC_MAX_MS (1).
# NO-GO is a warning, but aborts with --start-at. P5G_SYNC=off skips all of it.
#
# Binary: $P5G_SENDER_BIN, else ffmpeg/build/apps/video_sender, else ./video_sender next to this script.
#
# Layout: this script lives in ffmpeg/ (SMEC-style transport tree); results/, video/assets and
# scripts/setup are shared at the repo root, so the working directory is the repo root.
set -euo pipefail
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TREE/.." && pwd)"
cd "$ROOT"
HOST="${1:?usage: run_sender.sh <receiver-host> [--start-at T] [video_sender args...]}"; shift
BIN="${P5G_SENDER_BIN:-}"
[ -n "$BIN" ] || for c in "$TREE/build/apps/video_sender" "$TREE/video_sender"; do [ -x "$c" ] && { BIN="$c"; break; }; done
[ -n "$BIN" ] || { echo "video_sender binary not found (P5G_SENDER_BIN, ffmpeg/build/apps/, or next to this script)" >&2; exit 1; }

# script-only options; everything else goes to video_sender
START_AT=""; ARGS=(); EXPLICIT_YUV=0
while [ $# -gt 0 ]; do
  case "$1" in
    --start-at) START_AT="$2"; shift 2;;
    --start-at=*) START_AT="${1#--start-at=}"; shift;;
    --source|--source=*) EXPLICIT_YUV=1; ARGS+=("$1"); shift;;
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
RD="results/${P5G_RUN_ID:-$(date +%Y%m%d-%H%M%S)}-sender-$STREAM"
[ -e "$RD" ] && { echo "[sender] run dir $RD already exists (P5G_RUN_ID must be unique per launch)" >&2; exit 1; }
mkdir -p "$RD/app"
echo "[sender] route to $HOST: $(ip route get "$HOST" 2>/dev/null | head -1 || echo '(unknown)')"
echo "[sender] run dir: $RD   binary: $BIN"
{ date -u +"start_utc=%Y-%m-%dT%H:%M:%S.%NZ"; chronyc tracking 2>/dev/null || echo "chrony: not available"; } > "$RD/app/clock-$STREAM.txt"

# video source: the rungs of one pre-encoded content, "<path>@<kbps>,..." (prefix from P5G_SOURCE / camera id)
YUV_ARGS=()
if [ "$EXPLICIT_YUV" = 0 ]; then
  PREFIX="${P5G_SOURCE:-}"
  [ -n "$PREFIX" ] || { [ -n "$K" ] && ls "video/assets/kendo_view${K}_1280x720_30_"*k.h264 >/dev/null 2>&1 && PREFIX="kendo_view${K}_1280x720_30"; }
  [ -n "$PREFIX" ] || { ls video/assets/mot17-02_1280x720_30_*k.h264 >/dev/null 2>&1 && PREFIX="mot17-02_1280x720_30"; }
  [ -n "$PREFIX" ] || { echo "[sender] no pre-encoded rungs in video/assets (run ffmpeg/scripts/prepare_client.sh)" >&2; exit 1; }
  LIST=""
  for f in $(ls video/assets/${PREFIX}_*k.h264 | sort -t_ -k5 -n); do
    kb="$(basename "$f" | sed -E 's/.*_([0-9]+)k\.h264$/\1/')"
    if [ -n "${P5G_RUNGS:-}" ]; then case " $P5G_RUNGS " in *" $kb "*) ;; *) continue;; esac; fi
    LIST="${LIST:+$LIST,}$f@$kb"
  done
  YUV_ARGS=(--source "$LIST"); echo "[sender] source rungs: $LIST" | tee -a "$RD/app/clock-$STREAM.txt"
fi

# synchronised capture grid: the epoch goes into the app (it prepares and negotiates first, then waits for T)
EPOCH_ARGS=()
if [ -n "$START_AT" ]; then
  if [[ "$START_AT" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then TARGET="$START_AT"; else TARGET="$(date -d "today $START_AT" +%s.%N)" || { echo "bad --start-at $START_AT" >&2; exit 1; }; fi
  WAIT="$(awk -v t="$TARGET" -v n="$(date +%s.%N)" 'BEGIN{printf "%.3f", t-n}')"
  if [ "$(awk -v w="$WAIT" 'BEGIN{print (w<0)?1:0}')" = 1 ]; then echo "[sender] --start-at $START_AT is in the past (by ${WAIT#-} s); the grid keeps its phase, capture starts at the next slot" >&2
  else echo "[sender] capture epoch in ${WAIT} s at $(date -d "@$TARGET" +%H:%M:%S.%N) (the app waits after negotiating)"; fi
  echo "capture_epoch=$TARGET launched_at=$(date +%s.%N)" | tee -a "$RD/app/clock-$STREAM.txt"
  EPOCH_ARGS=(--start-at-epoch "$TARGET")
fi

"$BIN" --control-host "$HOST" --control-port "${P5G_CONTROL_PORT:-8765}" --trace-dir "$RD/app" \
       --fps 30 "${YUV_ARGS[@]}" "${EPOCH_ARGS[@]}" \
       --bitrate-kbps 2500 --duration 300 "$@" 2>&1 | (trap '' INT; exec tee "$RD/app/sender-$STREAM.log")
echo "[sender] done. traces: $RD/app"

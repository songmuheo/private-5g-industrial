#!/usr/bin/env bash
# Sender PC (laptop tethered to the Pixel): run video_sender against the receiver host.
#
#   ./run_sender.sh <receiver-host> [extra video_sender args...]
#
#   receiver-host : where ./run_receiver.sh runs (relay on TCP 8765). On the testbed that is the gNB
#                   PC's core-bridge address 10.53.1.1, reachable from the UE through the UPF.
#   extra args    : any video_sender flag; later flags override the defaults below, e.g.
#                   --width 1920 --height 1080 --fps 30 --yuv file.yuv --duration 120 --degradation stock
#   --start-at T  : (script option, not passed to video_sender) wait until wall-clock time T before starting,
#                   T = HH:MM:SS (today, local time) or a UNIX epoch (seconds, fractions allowed). Type the same
#                   T on every laptop to start all senders together; needs the laptops' clocks synced (chrony,
#                   docs/SETUP.md "Clock sync"). The wait and the actual start time are printed and logged.
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
# ---- ADDED: --start-at (synchronised start across laptops) ----
START_AT=""; ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --start-at) START_AT="$2"; shift 2;;
    --start-at=*) START_AT="${1#--start-at=}"; shift;;
    *) ARGS+=("$1"); shift;;
  esac
done
set -- "${ARGS[@]}"
# ---- END ADDED ----------------
STREAM=cam0; prev=""; for a in "$@"; do [ "$prev" = "--stream-id" ] && STREAM="$a"; prev="$a"; done
RD="results/$(date +%Y%m%d-%H%M%S)-sender-$STREAM"
mkdir -p "$RD/app"
echo "[sender] route to $HOST: $(ip route get "$HOST" 2>/dev/null | head -1 || echo '(unknown)')"
echo "[sender] run dir: $RD   binary: $BIN"
# ---- ADDED: clock-sync record + synchronised start ----
# chrony state at start (offset to the reference, if chrony runs): cross-host *_wall_ns columns are only
# comparable to this accuracy. Best effort; absent chrony leaves a note.
{ date -u +"start_utc=%Y-%m-%dT%H:%M:%S.%NZ"; chronyc tracking 2>/dev/null || echo "chrony: not available"; } > "$RD/app/clock-$STREAM.txt"
if [ -n "$START_AT" ]; then
  if [[ "$START_AT" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then TARGET="$START_AT"; else TARGET="$(date -d "today $START_AT" +%s.%N)" || { echo "bad --start-at $START_AT" >&2; exit 1; }; fi
  NOW="$(date +%s.%N)"; WAIT="$(awk -v t="$TARGET" -v n="$NOW" 'BEGIN{printf "%.3f", t-n}')"
  if [ "$(awk -v w="$WAIT" 'BEGIN{print (w<0)?1:0}')" = 1 ]; then echo "[sender] --start-at $START_AT is in the past (by ${WAIT#-} s); starting now" >&2
  else echo "[sender] waiting ${WAIT} s until $(date -d "@$TARGET" +%H:%M:%S.%N) ..."; sleep "$WAIT"; fi
  echo "[sender] started at $(date +%H:%M:%S.%N) (target $START_AT)" | tee -a "$RD/app/clock-$STREAM.txt"
fi
# ---- END ADDED ----------------
"$BIN" --signaling-host "$HOST" --signaling-port 8765 --trace-dir "$RD/app" \
       --codec H264 --width 1280 --height 720 --fps 30 \
       --degradation maintain_resolution --start-bitrate-kbps auto --duration 60 "$@" 2>&1 | (trap '' INT; exec tee "$RD/app/sender-$STREAM.log")
echo "[sender] done. traces: $RD/app"

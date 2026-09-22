#!/usr/bin/env bash
# Receiver host (the gNB PC or an internet host): signaling relay + N video_receiver processes.
#
#   ./run_receiver.sh [run_dir] [-n N] [extra video_receiver args...]
#
#   -n N     : number of receivers (default 1), one per UE, ids recv0 .. recv<N-1>. Each sender picks
#              its receiver with --to recvK and its own --stream-id (see run_sender.sh).
#   run_dir  : default = the run published by ./run_gnb_core.sh (results/CURRENT), else a new
#              results/<timestamp>-receiver. All receivers write into <run_dir>/app:
#              <stream>-rx-*.csv, <stream>-rx-stats.jsonl, receiver-recvK.log, signaling.log.
#   extra    : passed to every receiver (e.g. --low-latency-playout, --ice-servers ...). With -n 1 a
#              --receiver-id may be given to rename the single receiver.
#
# One receiver process per stream is a design rule (the decoder trace is per process); this script only
# saves the N terminals. The relay is started unless something already listens on 8765. The receivers'
# output is shown live (tail -F of their logs). Ctrl-C stops the receivers first (their traces flush and
# get their footer), then the relay.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
RD=""; N=1; RID_SINGLE=""; ARGS=()
if [ $# -gt 0 ] && [[ "$1" != -* ]]; then RD="$1"; shift; fi
while [ $# -gt 0 ]; do
  case "$1" in
    -n|--count) N="$2"; shift 2;;
    --receiver-id) RID_SINGLE="$2"; shift 2;;
    *) ARGS+=("$1"); shift;;
  esac
done
[[ "$N" =~ ^[1-9][0-9]*$ ]] || { echo "-n must be a positive integer" >&2; exit 1; }
[ "$N" -eq 1 ] || [ -z "$RID_SINGLE" ] || { echo "--receiver-id only makes sense with -n 1 (ids are recv0..recv$((N-1)))" >&2; exit 1; }
if [ -z "$RD" ]; then
  if [ -L results/CURRENT ]; then RD="results/$(readlink results/CURRENT)"; else RD="results/$(date +%Y%m%d-%H%M%S)-receiver"; fi
fi
mkdir -p "$RD/app"
[ -x build/apps/video_receiver ] || { echo "build/apps/video_receiver missing (make build-apps)" >&2; exit 1; }

RELAY_PID=""; PIDS=(); LOGS=()
cleanup() {
  set +e
  trap - INT TERM
  echo; echo "[receiver] stopping ${#PIDS[@]} receiver(s)..."
  for p in "${PIDS[@]}"; do kill -INT "$p" 2>/dev/null; done
  for p in "${PIDS[@]}"; do for i in $(seq 1 100); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done; kill -KILL "$p" 2>/dev/null; done
  [ -n "$RELAY_PID" ] && kill -TERM "$RELAY_PID" 2>/dev/null
  echo "[receiver] stopped. traces: $RD/app"
}
trap cleanup EXIT

if ss -ltn | grep -q ':8765 '; then
  echo "[receiver] signaling relay already listening on :8765 (reusing it)"
else
  nohup python3 apps/signaling/signaling_server.py --host 0.0.0.0 --port 8765 > "$RD/app/signaling.log" 2>&1 &
  RELAY_PID=$!
  sleep 1
fi
for i in $(seq 0 $((N - 1))); do
  RID="recv$i"; [ "$N" -eq 1 ] && [ -n "$RID_SINGLE" ] && RID="$RID_SINGLE"
  LOG="$RD/app/receiver-$RID.log"
  build/apps/video_receiver --signaling-host 127.0.0.1 --signaling-port 8765 --trace-dir "$RD/app" \
      --receiver-id "$RID" "${ARGS[@]}" > "$LOG" 2>&1 &
  PIDS+=($!); LOGS+=("$LOG")
  echo "[receiver] $RID pid=$! -> sender: ./run_sender.sh <host> --to $RID --stream-id cam$i"
done
echo "[receiver] run dir: $RD   (Ctrl-C to stop)"
tail -n +1 -F "${LOGS[@]}"

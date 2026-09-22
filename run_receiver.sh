#!/usr/bin/env bash
# Receiver host (the gNB PC or an internet host): signaling relay + video_receiver, in the foreground.
#
#   ./run_receiver.sh [run_dir] [extra video_receiver args...]
#
# run_dir: default = the run published by ./run_gnb_core.sh (results/CURRENT), else a new
# results/<timestamp>-receiver. Traces go to <run_dir>/app (rx-*.csv, rx-stats.jsonl, receiver.log,
# signaling.log). The relay is started only if nothing listens on 8765 yet. Ctrl-C stops both.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
RD=""
if [ $# -gt 0 ] && [[ "$1" != --* ]]; then RD="$1"; shift; fi
if [ -z "$RD" ]; then
  if [ -L results/CURRENT ]; then RD="results/$(readlink results/CURRENT)"; else RD="results/$(date +%Y%m%d-%H%M%S)-receiver"; fi
fi
mkdir -p "$RD/app"
[ -x build/apps/video_receiver ] || { echo "build/apps/video_receiver missing (make build-apps)" >&2; exit 1; }
RELAY_PID=""
cleanup() { set +e; [ -n "$RELAY_PID" ] && kill -TERM "$RELAY_PID" 2>/dev/null; echo "[receiver] stopped. traces: $RD/app"; }
trap cleanup EXIT

if ss -ltn | grep -q ':8765 '; then
  echo "[receiver] signaling relay already listening on :8765 (reusing it)"
else
  nohup python3 apps/signaling/signaling_server.py --host 0.0.0.0 --port 8765 > "$RD/app/signaling.log" 2>&1 &
  RELAY_PID=$!
  sleep 1
fi
echo "[receiver] run dir: $RD   (sender: ./run_sender.sh <this host's IP as seen from the UE>)"
# tee ignores SIGINT so a Ctrl-C reaches the app first and its output (and trace flush) completes
# before the pipe closes; otherwise the app dies of SIGPIPE mid-shutdown.
build/apps/video_receiver --signaling-host 127.0.0.1 --signaling-port 8765 --trace-dir "$RD/app" "$@" 2>&1 | (trap '' INT; exec tee "$RD/app/receiver.log")

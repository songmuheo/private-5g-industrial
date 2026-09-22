#!/usr/bin/env bash
# gNB PC: start the 5G side by hand — Open5GS core + srsRAN gNB (tracer on) + JSON metrics client.
#
#   ./run_gnb_core.sh [label]        -> results/<timestamp>-<label>/{gnb,core}   (default label: ota)
#
# Runs the gNB in the foreground (its stdout metrics table on screen and in gnb/gnb_stdout.log).
# Ctrl-C stops the gNB (traces flush), saves the core log, and takes the core down.
# The run directory is published in results/CURRENT so ./run_receiver.sh on this PC writes into
# the same run without any shared shell variables.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
LABEL="${1:-ota}"
RD="results/$(date +%Y%m%d-%H%M%S)-${LABEL}"
mkdir -p "$RD/gnb" "$RD/app" "$RD/core"
ln -sfn "$(basename "$RD")" results/CURRENT
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3
METRICS_PID=""

cleanup() {
  set +e
  echo; echo "[gnb_core] stopping..."
  [ -n "$METRICS_PID" ] && kill -TERM "$METRICS_PID" 2>/dev/null
  sudo pkill -TERM -x gnb 2>/dev/null; sleep 3; sudo pkill -KILL -x gnb 2>/dev/null
  docker logs p5g_open5gs > "$RD/core/open5gs.log" 2>&1
  scripts/run/start_core.sh down >/dev/null 2>&1
  sudo chown -R "$(id -u):$(id -g)" "$RD" 2>/dev/null
  echo "[gnb_core] stopped. run dir: $RD"
}
trap cleanup EXIT

echo "[gnb_core] run dir: $RD  (results/CURRENT -> $(basename "$RD"))"
scripts/run/start_core.sh
nohup "$PY" ran/gnb/metrics_json_client.py --out "$RD/gnb/gnb_metrics.jsonl" > "$RD/gnb/metrics_json_client.log" 2>&1 &
METRICS_PID=$!
cat <<MSG
[gnb_core] next, in other terminals:
  receiver (this PC):   ./run_receiver.sh                # relay :8765 + video_receiver -> $RD/app
  sender (UE laptop):   ./run_sender.sh 10.53.1.1        # after the phone is attached
  stop:                 Ctrl-C here
MSG
# tee ignores SIGINT: Ctrl-C goes to the gNB, which flushes its traces and exits; only then does the pipe close.
scripts/run/run_gnb.sh b210_n78_tdd_20mhz "$RD/gnb" 2>&1 | (trap '' INT; exec tee "$RD/gnb/gnb_stdout.log")

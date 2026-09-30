#!/usr/bin/env bash
# gNB-PC convenience: (re)start the whole gNB side for an over-the-air session and keep it running.
#   scripts/run/ota_restart.sh [label]      -> results/<timestamp>-<label>/{gnb,app,core}
#   scripts/run/ota_restart.sh stop         -> stop gNB, receiver, signaling relay, metrics client, core
# Starts: Open5GS (+ P5G_UE_DNNS), gNB (b210 profile, tracer on), JSON metrics client, and the transport
# tree's receivers (<tree>/run_receiver.sh -n P5G_RECEIVERS, default 1; P5G_TREE selects gstreamer|webrtc).
# PIDs are kept in <run>/pids so stop is exact.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

stop_all() {
  local rd; rd="$(cat build/ota_rd.txt 2>/dev/null || true)"
  if [ -n "$rd" ] && [ -d "$rd/pids" ]; then
    for f in "$rd"/pids/*.pid; do [ -f "$f" ] && sudo kill -TERM "$(cat "$f")" 2>/dev/null || true; done
  fi
  # Also catch instances started by hand (this script's own cmdline never contains these names).
  sudo pkill -TERM -x gnb 2>/dev/null || true
  pkill -TERM -x video_receiver 2>/dev/null || true
  pkill -TERM -f "[s]ignaling_server.py" 2>/dev/null || true
  pkill -TERM -f "[m]etrics_json_client.py" 2>/dev/null || true
  sleep 4
  sudo pkill -KILL -x gnb 2>/dev/null || true
  scripts/run/start_core.sh down >/dev/null 2>&1 || true
  [ -n "$rd" ] && [ -d "$rd" ] && sudo chown -R "$(id -u):$(id -g)" "$rd" 2>/dev/null || true
  echo "[ota] stopped"
}

if [ "${1:-}" = "stop" ]; then stop_all; exit 0; fi
stop_all >/dev/null 2>&1 || true

LABEL="${1:-ota}"
RD="results/$(date +%Y%m%d-%H%M%S)-${LABEL}"
mkdir -p "$RD/gnb" "$RD/core" "$RD/app" "$RD/pids"
echo "$RD" > build/ota_rd.txt

scripts/run/start_core.sh | tail -3
nohup scripts/run/run_gnb.sh b210_n78_tdd_20mhz "$RD/gnb" > "$RD/gnb/gnb_stdout.log" 2>&1 &
echo $! > "$RD/pids/gnb.pid"
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3
nohup "$PY" ran/gnb/metrics_json_client.py --out "$RD/gnb/gnb_metrics.jsonl" > "$RD/gnb/metrics_json_client.log" 2>&1 &
echo $! > "$RD/pids/metrics.pid"
TREE="$ROOT/${P5G_TREE:-gstreamer}"
[ -x "$TREE/run_receiver.sh" ] || { echo "unknown transport tree ${P5G_TREE:-gstreamer} ($TREE/run_receiver.sh missing)" >&2; exit 1; }
P5G_RECEIVER_NOTAIL=1 nohup "$TREE/run_receiver.sh" "$RD" -n "${P5G_RECEIVERS:-1}" > "$RD/app/run_receiver.log" 2>&1 &
echo $! > "$RD/pids/receiver.pid"
sleep 2

until grep -q "gNB started" "$RD/gnb/gnb_stdout.log" 2>/dev/null || grep -q "srsRAN ERROR" "$RD/gnb/gnb_stdout.log" 2>/dev/null; do sleep 2; done
grep -E "Operating over|Cell pci|N2:|gNB started|ERROR" "$RD/gnb/gnb_stdout.log" | cut -c1-120
echo "[ota] run dir: $RD  (sender: $TREE/run_sender.sh 10.53.1.1 --to recv0 ...)"

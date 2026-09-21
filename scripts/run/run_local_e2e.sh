#!/usr/bin/env bash
# CODE-TEST TOOL: exercise the whole chain on one PC without radios.
#   Open5GS + srsRAN gNB (ZeroMQ profile, tracer on) + srsUE (netns ue1) + video_sender in the UE
#   namespace -> video_receiver on the host (uplink; --direction dl swaps the two apps).
# Every component is started exactly as on the real testbed (same binaries, same scripts); only the
# radio is replaced by the ZeroMQ baseband loopback and the UE by srsUE.
#
#   scripts/run/run_local_e2e.sh [--duration S] [--codec H264] [--width W --height H --fps F]
#                                [--yuv FILE] [--label NAME] [--direction ul|dl]
#
# Output: results/<timestamp>-<label>/{gnb,ue,app,core}/ + run.json, then analysis/verify_run.py.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

DURATION=30; CODEC=H264; WIDTH=1280; HEIGHT=720; FPS=30; YUV=""; LABEL="local"; DIRECTION="ul"
WARMUP=5; DRAIN=3
while [ $# -gt 0 ]; do
  case "$1" in
    --duration) DURATION="$2"; shift 2;;
    --codec) CODEC="$2"; shift 2;;
    --width) WIDTH="$2"; shift 2;;
    --height) HEIGHT="$2"; shift 2;;
    --fps) FPS="$2"; shift 2;;
    --yuv) YUV="$2"; shift 2;;
    --label) LABEL="$2"; shift 2;;
    --direction) DIRECTION="$2"; shift 2;;
    *) echo "unknown arg $1" >&2; exit 1;;
  esac
done

RUN_ID="$(date +%Y%m%d-%H%M%S)-${LABEL}"
RD="$ROOT/results/$RUN_ID"
mkdir -p "$RD/gnb" "$RD/ue" "$RD/app" "$RD/core"
echo "[e2e] run dir: $RD"
SENDER="$ROOT/build/apps/video_sender"; RECEIVER="$ROOT/build/apps/video_receiver"
[ -x "$SENDER" ] && [ -x "$RECEIVER" ] || { echo "apps not built (scripts/build/build_apps.sh)" >&2; exit 1; }
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3

PIDS=()
cleanup() {
  set +e
  echo "[e2e] stopping components..."
  for p in "${PIDS[@]:-}"; do [ -n "$p" ] && sudo kill -TERM "$p" 2>/dev/null; done
  sleep 3
  sudo pkill -TERM -x srsue 2>/dev/null; sleep 2
  sudo pkill -TERM -x gnb 2>/dev/null; sleep 3
  for p in "${PIDS[@]:-}"; do [ -n "$p" ] && sudo kill -KILL "$p" 2>/dev/null; done
  sudo chown -R "$(id -u):$(id -g)" "$RD" 2>/dev/null
}
trap cleanup EXIT

# 1) core  2) gNB (+ JSON metrics client)  3) srsUE
"$ROOT/scripts/run/start_core.sh"
"$ROOT/scripts/run/run_gnb.sh" zmq_local "$RD/gnb" > "$RD/gnb/gnb_stdout.log" 2>&1 &
PIDS+=($!)
sleep 4
pgrep -x gnb >/dev/null || { echo "[e2e] gNB failed to start:"; tail -30 "$RD/gnb/gnb_stdout.log"; exit 1; }
"$PY" "$ROOT/ran/gnb/metrics_json_client.py" --url ws://127.0.0.1:8001 --out "$RD/gnb/gnb_metrics.jsonl" > "$RD/gnb/metrics_json_client.log" 2>&1 &
PIDS+=($!)
"$ROOT/scripts/run/run_ue_sim.sh" "$RD/ue" > "$RD/ue/ue_stdout.log" 2>&1 &
PIDS+=($!)

echo "[e2e] waiting for UE attach..."
UE_IP=""
for i in $(seq 1 60); do
  UE_IP="$( (sudo ip netns exec ue1 ip -4 -o addr show dev tun_srsue 2>/dev/null || true) | awk '{print $4}' | cut -d/ -f1)"
  [ -n "$UE_IP" ] && break
  sleep 1
done
[ -n "$UE_IP" ] || { echo "[e2e] UE did not attach"; tail -20 "$RD/ue/ue_stdout.log"; exit 1; }
echo "[e2e] UE attached: $UE_IP"
sudo ip netns exec ue1 ip route replace default dev tun_srsue
HOST_IP=10.53.1.1   # host side of the core bridge: reachable from the UE through the UPF

# 4) signaling relay + apps (receiver side = host, sender side = UE namespace for uplink)
python3 "$ROOT/apps/signaling/signaling_server.py" --host 0.0.0.0 --port 8765 > "$RD/app/signaling.log" 2>&1 &
PIDS+=($!)
sleep 1
APP=(--session s1 --signaling-port 8765 --trace-dir "$RD/app")
VID=(--codec "$CODEC" --width "$WIDTH" --height "$HEIGHT" --fps "$FPS"); [ -n "$YUV" ] && VID+=(--yuv "$YUV")
TOTAL=$((WARMUP + DURATION + DRAIN))
if [ "$DIRECTION" = "ul" ]; then
  "$RECEIVER" "${APP[@]}" --signaling-host 127.0.0.1 --receiver-id recv0 --duration $((TOTAL + 5)) > "$RD/app/receiver.log" 2>&1 &
  PIDS+=($!); sleep 1
  sudo -E ip netns exec ue1 "$SENDER" "${APP[@]}" "${VID[@]}" --signaling-host "$HOST_IP" --stream-id cam0 --to recv0 --duration "$TOTAL" > "$RD/app/sender.log" 2>&1 &
  PIDS+=($!)
else
  sudo -E ip netns exec ue1 "$RECEIVER" "${APP[@]}" --signaling-host "$HOST_IP" --receiver-id recv0 --duration $((TOTAL + 5)) > "$RD/app/receiver.log" 2>&1 &
  PIDS+=($!); sleep 1
  "$SENDER" "${APP[@]}" "${VID[@]}" --signaling-host 127.0.0.1 --stream-id cam0 --to recv0 --duration "$TOTAL" > "$RD/app/sender.log" 2>&1 &
  PIDS+=($!)
fi

# 5) measurement window (boundaries recorded in run.json; every log is continuous)
sleep "$WARMUP"; WIN_START_NS=$(date +%s%N)
echo "[e2e] measurement window: ${DURATION}s"
sleep "$DURATION"; WIN_END_NS=$(date +%s%N)
sleep "$DRAIN"

# 6) stop: apps first (flush), then RAN, then collect the core log
sudo pkill -TERM -x video_sender || true; sleep 2
pkill -TERM -x video_receiver || true; sleep 2
cleanup; trap - EXIT
docker logs p5g_open5gs > "$RD/core/open5gs.log" 2>&1 || true

cat > "$RD/run.json" <<EOF
{
  "run_id": "$RUN_ID", "mode": "local_zmq_codetest", "direction": "$DIRECTION",
  "duration_s": $DURATION, "warmup_s": $WARMUP, "drain_s": $DRAIN,
  "window_start_wall_ns": $WIN_START_NS, "window_end_wall_ns": $WIN_END_NS,
  "codec": "$CODEC", "width": $WIDTH, "height": $HEIGHT, "fps": $FPS, "source": "${YUV:-pattern}",
  "ue_ip": "$UE_IP",
  "srsran_project": "$(git -C "$ROOT/third_party/srsRAN_Project" describe --tags --always)",
  "srsran_4g": "$(git -C "$ROOT/third_party/srsRAN_4G" describe --tags --always)",
  "libwebrtc": "$(grep libwebrtc_commit "$ROOT/build/apps/BUILD_INFO.txt" | cut -d= -f2)"
}
EOF
"$PY" "$ROOT/analysis/verify_run.py" "$RD" || true
echo "[e2e] done: $RD"

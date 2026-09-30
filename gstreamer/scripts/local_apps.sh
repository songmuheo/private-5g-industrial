#!/usr/bin/env bash
# App phase of the one-PC code test (scripts/run/run_local_e2e.sh) for the gstreamer stack.
# Contract (same for every transport tree; the RAN part is started by the shared script):
#
#   <tree>/scripts/local_apps.sh start <run_dir> <ue_ip> <host_ip> <total_s> <ul|dl> <codec> <W> <H> <fps> [yuv]
#   <tree>/scripts/local_apps.sh stop  <run_dir>
#
# start: control server + receiver on the host side, sender in netns ue1 (uplink) or the reverse (dl);
#        PIDs -> <run_dir>/app/pids.txt, provenance -> <run_dir>/app/build_info.json; returns.
# stop : graceful stop in flush order (sender, receiver, control server) so every trace gets its footer.
set -euo pipefail
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMD="${1:?start|stop}"; RD="${2:?run_dir}"
SENDER="$TREE/build/apps/video_sender"; RECEIVER="$TREE/build/apps/video_receiver"

if [ "$CMD" = "stop" ]; then
  sudo pkill -TERM -x video_sender || true; sleep 2
  pkill -TERM -x video_receiver || true; sleep 2
  pkill -TERM -f "[c]ontrol_server.py" || true
  exit 0
fi
[ "$CMD" = "start" ] || { echo "usage: local_apps.sh start|stop ..." >&2; exit 1; }
UE_IP="${3:?ue_ip}"; HOST_IP="${4:?host_ip}"; TOTAL="${5:?total_s}"; DIRECTION="${6:?ul|dl}"
CODEC="${7:?codec}"; WIDTH="${8:?W}"; HEIGHT="${9:?H}"; FPS="${10:?fps}"; YUV="${11:-}"
[ "$CODEC" = "H264" ] || { echo "gstreamer tree: only H264 (x264enc) is built in; got $CODEC" >&2; exit 1; }
[ -x "$SENDER" ] && [ -x "$RECEIVER" ] || { echo "gstreamer apps not built (make build-apps)" >&2; exit 1; }

PIDS=()
APP=(--session s1 --control-port 8765 --trace-dir "$RD/app")
VID=(--width "$WIDTH" --height "$HEIGHT" --fps "$FPS"); [ -n "$YUV" ] && VID+=(--yuv "$YUV")
if [ "$DIRECTION" = "ul" ]; then
  # control server + receiver on the host (reachable from the UE at HOST_IP through the UPF)
  python3 "$TREE/apps/control/control_server.py" --host 0.0.0.0 --port 8765 > "$RD/app/control.log" 2>&1 &
  PIDS+=($!); sleep 1
  "$RECEIVER" "${APP[@]}" --control-host 127.0.0.1 --receiver-id recv0 --advertise-host "$HOST_IP" --duration $((TOTAL + 5)) > "$RD/app/receiver.log" 2>&1 &
  PIDS+=($!); sleep 1
  sudo -E ip netns exec ue1 "$SENDER" "${APP[@]}" "${VID[@]}" --control-host "$HOST_IP" --stream-id cam0 --to recv0 --duration "$TOTAL" > "$RD/app/sender.log" 2>&1 &
  PIDS+=($!)
else
  # downlink: control server + receiver in the UE namespace, sender on the host; the receiver advertises its UE address
  sudo -E ip netns exec ue1 python3 "$TREE/apps/control/control_server.py" --host 0.0.0.0 --port 8765 > "$RD/app/control.log" 2>&1 &
  PIDS+=($!); sleep 1
  sudo -E ip netns exec ue1 "$RECEIVER" "${APP[@]}" --control-host 127.0.0.1 --receiver-id recv0 --advertise-host "$UE_IP" --duration $((TOTAL + 5)) > "$RD/app/receiver.log" 2>&1 &
  PIDS+=($!); sleep 1
  "$SENDER" "${APP[@]}" "${VID[@]}" --control-host "$UE_IP" --stream-id cam0 --to recv0 --duration "$TOTAL" > "$RD/app/sender.log" 2>&1 &
  PIDS+=($!)
fi
printf '%s\n' "${PIDS[@]}" > "$RD/app/pids.txt"
python3 - "$TREE/build/apps/BUILD_INFO.txt" > "$RD/app/build_info.json" <<'PY'
import json, sys
kv = dict(l.rstrip('\n').split('=', 1) for l in open(sys.argv[1]) if '=' in l)
print(json.dumps(kv))
PY

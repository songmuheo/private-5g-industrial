#!/usr/bin/env bash
# App phase of the one-PC code test (scripts/run/run_local_e2e.sh) for the webrtc stack.
# Contract shared by every transport tree (webrtc/, gstreamer/): the RAN part (core, gNB, srsUE in
# netns ue1) is started by the shared script, which then calls
#
#   <tree>/scripts/local_apps.sh start <run_dir> <ue_ip> <host_ip> <total_s> <ul|dl> <codec> <W> <H> <fps> [yuv]
#   <tree>/scripts/local_apps.sh stop  <run_dir>
#
# start: launch the tree's relay/receiver/sender (sender in the UE namespace for uplink), record their
#        PIDs in <run_dir>/app/pids.txt and the build provenance in <run_dir>/app/build_info.json, return.
# stop : graceful stop in flush order (sender, receiver, relay) so every trace gets its footer.
# Extracted unchanged from the former run_local_e2e.sh step 4 (libwebrtc M120 sender/receiver).
set -euo pipefail
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMD="${1:?start|stop}"; RD="${2:?run_dir}"
SENDER="$TREE/build/apps/video_sender"; RECEIVER="$TREE/build/apps/video_receiver"

if [ "$CMD" = "stop" ]; then
  sudo pkill -TERM -x video_sender || true; sleep 2
  pkill -TERM -x video_receiver || true; sleep 2
  pkill -TERM -f "[s]ignaling_server.py" || true
  exit 0
fi
[ "$CMD" = "start" ] || { echo "usage: local_apps.sh start|stop ..." >&2; exit 1; }
UE_IP="${3:?ue_ip}"; HOST_IP="${4:?host_ip}"; TOTAL="${5:?total_s}"; DIRECTION="${6:?ul|dl}"
CODEC="${7:?codec}"; WIDTH="${8:?W}"; HEIGHT="${9:?H}"; FPS="${10:?fps}"; YUV="${11:-}"
[ -x "$SENDER" ] && [ -x "$RECEIVER" ] || { echo "webrtc apps not built (webrtc/scripts/build_apps.sh)" >&2; exit 1; }

PIDS=()
python3 "$TREE/apps/signaling/signaling_server.py" --host 0.0.0.0 --port 8765 > "$RD/app/signaling.log" 2>&1 &
PIDS+=($!)
sleep 1
APP=(--session s1 --signaling-port 8765 --trace-dir "$RD/app")
VID=(--codec "$CODEC" --width "$WIDTH" --height "$HEIGHT" --fps "$FPS"); [ -n "$YUV" ] && VID+=(--yuv "$YUV")
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
printf '%s\n' "${PIDS[@]}" > "$RD/app/pids.txt"
printf '{ "transport": "webrtc", "libwebrtc": "%s" }\n' "$(grep libwebrtc_commit "$TREE/build/apps/BUILD_INFO.txt" | cut -d= -f2)" > "$RD/app/build_info.json"

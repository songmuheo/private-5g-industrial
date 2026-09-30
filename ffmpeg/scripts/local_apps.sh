#!/usr/bin/env bash
# App phase of the one-PC code test (scripts/run/run_local_e2e.sh) for the ffmpeg stack (pre-encoded RTP).
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

# stop: only this run's processes (pids.txt: "sender=PID receiver=PID control=PID"), in flush order —
# TERM the sender and WAIT for it to exit (it drains the encoder, EOS, footers), then the receiver, then the
# control server. sudo for every kill: the UE-namespace side was started with sudo.
if [ "$CMD" = "stop" ]; then
  PIDF="$RD/app/pids.txt"; [ -f "$PIDF" ] || { echo "no $PIDF" >&2; exit 1; }
  pid_of() { sed -n "s/^$1=//p" "$PIDF"; }
  wait_gone() { for i in $(seq 1 "$2"); do sudo kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done; return 1; }
  for role in sender receiver control; do
    p="$(pid_of $role)"; [ -n "$p" ] || continue
    sudo kill -TERM "$p" 2>/dev/null || true
    wait_gone "$p" 100 || { echo "[local_apps] $role (pid $p) did not exit in 10 s -> KILL" >&2; sudo kill -KILL "$p" 2>/dev/null || true; }
  done
  exit 0
fi
[ "$CMD" = "start" ] || { echo "usage: local_apps.sh start|stop ..." >&2; exit 1; }
UE_IP="${3:?ue_ip}"; HOST_IP="${4:?host_ip}"; TOTAL="${5:?total_s}"; DIRECTION="${6:?ul|dl}"
CODEC="${7:?codec}"; WIDTH="${8:?W}"; HEIGHT="${9:?H}"; FPS="${10:?fps}"; YUV="${11:-}"
[ "$CODEC" = "H264" ] || { echo "ffmpeg tree: pre-encoded H.264 only; got $CODEC" >&2; exit 1; }
[ -x "$SENDER" ] && [ -x "$RECEIVER" ] || { echo "ffmpeg apps not built (make build-apps TREE=ffmpeg)" >&2; exit 1; }

PORT="${P5G_CONTROL_PORT:-8765}"
APP=(--session s1 --control-port "$PORT" --trace-dir "$RD/app")
# source: P5G_SOURCE prefix (default: Kendo view 0 rungs, else MOT17); WIDTH/HEIGHT are those of the file
PREFIX="${P5G_SOURCE:-}"; [ -n "$PREFIX" ] || { ls "$TREE/../video/assets/kendo_view0_1280x720_30_"*k.h264 >/dev/null 2>&1 && PREFIX="kendo_view0_1280x720_30"; }
[ -n "$PREFIX" ] || PREFIX="mot17-02_1280x720_30"
LIST=""; for f in $(ls "$TREE/../video/assets/${PREFIX}_"*k.h264 2>/dev/null | sort -t_ -k5 -n); do kb="$(basename "$f" | sed -E 's/.*_([0-9]+)k\.h264$/\1/')"; LIST="${LIST:+$LIST,}$f@$kb"; done
[ -n "$LIST" ] || { echo "no pre-encoded rungs for $PREFIX in video/assets (ffmpeg/scripts/prepare_video.sh)" >&2; exit 1; }
VID=(--fps "$FPS" --source "$LIST" --bitrate-kbps "${P5G_START_KBPS:-2500}")
if [ "$DIRECTION" = "ul" ]; then
  # control server + receiver on the host (reachable from the UE at HOST_IP through the UPF)
  python3 "$TREE/apps/control/control_server.py" --host 0.0.0.0 --port "$PORT" > "$RD/app/control.log" 2>&1 &
  CONTROL=$!; sleep 1
  "$RECEIVER" "${APP[@]}" --control-host 127.0.0.1 --receiver-id recv0 --advertise-host "$HOST_IP" --duration $((TOTAL + 5)) > "$RD/app/receiver.log" 2>&1 &
  RECV=$!; sleep 1
  sudo -E ip netns exec ue1 "$SENDER" "${APP[@]}" "${VID[@]}" --control-host "$HOST_IP" --stream-id cam0 --to recv0 --duration "$TOTAL" > "$RD/app/sender.log" 2>&1 &
  SEND=$!
else
  # downlink: control server + receiver in the UE namespace, sender on the host; the receiver advertises its UE address
  sudo -E ip netns exec ue1 python3 "$TREE/apps/control/control_server.py" --host 0.0.0.0 --port "$PORT" > "$RD/app/control.log" 2>&1 &
  CONTROL=$!; sleep 1
  sudo -E ip netns exec ue1 "$RECEIVER" "${APP[@]}" --control-host 127.0.0.1 --receiver-id recv0 --advertise-host "$UE_IP" --duration $((TOTAL + 5)) > "$RD/app/receiver.log" 2>&1 &
  RECV=$!; sleep 1
  "$SENDER" "${APP[@]}" "${VID[@]}" --control-host "$UE_IP" --stream-id cam0 --to recv0 --duration "$TOTAL" > "$RD/app/sender.log" 2>&1 &
  SEND=$!
fi
# The sudo'd processes: $! is the sudo wrapper; TERM to it is forwarded to the child (sudo relays signals).
printf 'sender=%s\nreceiver=%s\ncontrol=%s\n' "$SEND" "$RECV" "$CONTROL" > "$RD/app/pids.txt"
python3 - "$TREE/build/apps/BUILD_INFO.txt" > "$RD/app/build_info.json" <<'PY'
import json, sys
kv = dict(l.rstrip('\n').split('=', 1) for l in open(sys.argv[1]) if '=' in l)
print(json.dumps(kv))
PY

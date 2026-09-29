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
# Defaults for a fixed-resolution experiment: H264 1280x720@30 from video/assets/kendo_view<K>_1280x720_30.yuv for
# --stream-id cam<K> (Nagoya "Kendo" multi-view: five laptops = five real camera angles of one scene, 10 s looped;
# get it with video/fetch_asset.sh), else the first of fade_walk / crowd_run / FourPeople, else the synthetic pattern
# with a loud warning,
# --degradation maintain_resolution, --start-bitrate-kbps auto (= 900 kbps for 720p H264, logged), 300 s.
# Traces -> results/<ts>-sender-<stream>/app
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
# ---- ADDED: clock sync at the start of every run (no separate setup step) ----
# P5G_SYNC=auto (default): if this laptop is not yet on the sync LAN (no NetworkManager connection "p5g-sync"),
#   run scripts/setup/sync_lan_client.sh <K> once (auto-detects the wired port on the switch, static
#   192.168.77.1K, installs/configures chrony -> gNB PC; asks for sudo). Then check chrony: RMS offset must be
#   <= P5G_SYNC_MAX_MS (default 1 ms). NO-GO only warns, except with --start-at where it aborts (the start
#   time would be meaningless). P5G_SYNC=off skips all of this (e.g. a laptop without a wired port).
P5G_SYNC="${P5G_SYNC:-auto}"; P5G_SYNC_MAX_MS="${P5G_SYNC_MAX_MS:-1}"
if [ "$P5G_SYNC" != "off" ]; then
  K="${STREAM//[!0-9]/}"
  if ! nmcli -t -f NAME con show 2>/dev/null | grep -qx p5g-sync; then
    if [ -n "$K" ]; then echo "[sender] sync LAN not configured on this laptop -> scripts/setup/sync_lan_client.sh $K"; scripts/setup/sync_lan_client.sh "$K" || echo "[sender] WARNING: sync-LAN setup failed; continuing without clock sync" >&2
    else echo "[sender] WARNING: sync LAN not configured and no camera id in --stream-id $STREAM; skipping clock sync" >&2; fi
  fi
  if scripts/setup/sync_check.sh "$P5G_SYNC_MAX_MS" 2>/dev/null | sed 's/^/[sync] /' | grep -E "GO|NO-GO|RMS|rtt"; then :; fi
  if ! scripts/setup/sync_check.sh "$P5G_SYNC_MAX_MS" >/dev/null 2>&1; then
    if [ -n "${START_AT:-}" ]; then echo "[sender] clock sync NO-GO (RMS offset > $P5G_SYNC_MAX_MS ms) and --start-at given -> aborting; wait for chrony or set P5G_SYNC=off" >&2; exit 1
    else echo "[sender] WARNING: clock sync NO-GO (RMS offset > $P5G_SYNC_MAX_MS ms); cross-host timestamps of this run are not trustworthy" >&2; fi
  fi
fi
# ---- END ADDED ----------------
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
# ---- EDITED: real test sequence and 300 s by default (were: synthetic pattern, 60 s) ----
# ===================== ORIGINAL (preserved) =====================
# "$BIN" --signaling-host "$HOST" --signaling-port 8765 --trace-dir "$RD/app" \
#        --codec H264 --width 1280 --height 720 --fps 30 \
#        --degradation maintain_resolution --start-bitrate-kbps auto --duration 60 "$@" 2>&1 | (trap '' INT; exec tee "$RD/app/sender-$STREAM.log")
# ===============================================================
# Default source: the first of these present in video/assets/ (all 1280x720 30 fps I420; video_sender loops them):
#   fade_walk_..._300s (fetch_asset.sh copy from the gNB PC) > fade_walk-web_..._300s (prepare_fade_walk.sh, from
#   YouTube) > crowd_run (10 s, prepare_test_sequence.sh) > FourPeople (10 s). Explicit --yuv wins.
#   Multi-camera content first: --stream-id camK -> video/assets/kendo_viewK_1280x720_30.yuv if present (Nagoya
#   "Kendo", 7 synchronized cameras of one scene; video/prepare_kendo.sh), so five senders play five real angles.
YUV_ARGS=(); YUV_DEFAULT=""
K="${STREAM//[!0-9]/}"
[ -n "$K" ] && [ -f "$ROOT/video/assets/kendo_view${K}_1280x720_30.yuv" ] && YUV_DEFAULT="$ROOT/video/assets/kendo_view${K}_1280x720_30.yuv"
[ -n "$YUV_DEFAULT" ] || for cand in fade_walk_1280x720_30fps_300s_i420.yuv fade_walk-web_1280x720_30fps_300s_i420.yuv crowd_run_1280x720_30fps_i420.yuv FourPeople_1280x720_30fps_i420.yuv; do
  [ -f "$ROOT/video/assets/$cand" ] && { YUV_DEFAULT="$ROOT/video/assets/$cand"; break; }
done
if printf '%s\n' "$@" | grep -qx -- '--yuv\|--yuv=.*'; then :   # caller chose a source explicitly
elif [ -n "$YUV_DEFAULT" ]; then YUV_ARGS=(--yuv "$YUV_DEFAULT"); echo "[sender] source: $YUV_DEFAULT"
else echo "[sender] WARNING: no .yuv in $ROOT/video/assets -> synthetic pattern (run video/fetch_asset.sh on this laptop)" | tee -a "$RD/app/clock-$STREAM.txt" >&2; fi
"$BIN" --signaling-host "$HOST" --signaling-port 8765 --trace-dir "$RD/app" \
       --codec H264 --width 1280 --height 720 --fps 30 "${YUV_ARGS[@]}" \
       --degradation maintain_resolution --start-bitrate-kbps auto --duration 300 "$@" 2>&1 | (trap '' INT; exec tee "$RD/app/sender-$STREAM.log")
# ---------------------------------------------------------------------------------------
echo "[sender] done. traces: $RD/app"

#!/usr/bin/env bash
# Run srsUE (ZeroMQ) inside network namespace `ue1` — CODE-TEST TOOL ONLY (no radio).
#   scripts/run/run_ue_sim.sh <log_dir>
# The UE tunnel (tun_srsue, 10.45.x.y) appears inside `ue1`; run the sender with
#   sudo ip netns exec ue1 build/apps/video_sender ...
# Stock srsUE outputs: ue.log, 1 s metrics CSV (rsrp, dl/ul mcs, brate, bler, ta, ul buffer), MAC-NR pcap.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="${1:?usage: run_ue_sim.sh <log_dir>}"
UE="$ROOT/build/srsran_ue_sim/srsue/src/srsue"
CFG="$ROOT/ran/ue_sim/configs/ue_zmq_local.conf"
[ -x "$UE" ] || { echo "srsUE not built: scripts/build/build_srsran_ue_sim.sh" >&2; exit 1; }
mkdir -p "$LOG_DIR"
ip netns list | grep -qw ue1 || sudo ip netns add ue1
echo "[run_ue_sim] srsUE (ZMQ, netns ue1) -> $LOG_DIR"
exec sudo "$UE" "$CFG" --log.filename "$LOG_DIR/ue.log" \
  --general.metrics_csv_enable=true --general.metrics_csv_filename "$LOG_DIR/ue_metrics.csv" \
  --general.metrics_period_secs=1 \
  --pcap.enable=mac_nr --pcap.mac_nr_filename "$LOG_DIR/ue_mac_nr.pcap"

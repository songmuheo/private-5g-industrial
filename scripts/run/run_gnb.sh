#!/usr/bin/env bash
# Run the (tracer-patched) srsRAN gNB with a config profile and a trace directory.
#
#   scripts/run/run_gnb.sh <profile> <trace_dir> [extra gnb args...]
#     profile   : name of ran/gnb/configs/gnb_<profile>.yml  (b210_n78_tdd_20mhz | zmq_local = code test)
#     trace_dir : directory for gnb_*.csv (P5G_GNB_TRACE_DIR) and gnb.log
#
# The gNB needs CAP_SYS_NICE / real-time scheduling for its threads -> runs under sudo, but keeps
# the caller's environment for the tracer variables. Ctrl-C stops it cleanly (traces are flushed).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROFILE="${1:?usage: run_gnb.sh <profile> <trace_dir>}"
TRACE_DIR="${2:?usage: run_gnb.sh <profile> <trace_dir>}"
shift 2
CFG="$ROOT/ran/gnb/configs/gnb_${PROFILE}.yml"
GNB="$ROOT/build/srsran_gnb/apps/gnb/gnb"
[ -f "$CFG" ] || { echo "no such profile: $CFG" >&2; exit 1; }
[ -x "$GNB" ] || { echo "gNB not built: run scripts/build/build_srsran_gnb.sh" >&2; exit 1; }
mkdir -p "$TRACE_DIR"

# Stock srsRAN pcaps (ground truth next to the tracer): MAC (DLT format, includes RLC PDUs), NGAP,
# N3 GTP-U (every user-plane IP packet with the CU-UP timestamp). Off by default: pcap writing adds
# CPU/disk load on the gNB host. Opt in with P5G_GNB_PCAP=1 for ground-truth captures.
PCAP_ARGS=()
if [ "${P5G_GNB_PCAP:-0}" = "1" ]; then
  PCAP_ARGS=(pcap --mac_enable true --mac_type dlt --mac_filename "$TRACE_DIR/gnb_mac.pcap"
                  --ngap_enable true --ngap_filename "$TRACE_DIR/gnb_ngap.pcap"
                  --n3_enable true --n3_filename "$TRACE_DIR/gnb_n3_gtpu.pcap")
fi
echo "[run_gnb] profile=$PROFILE traces=$TRACE_DIR pcap=${P5G_GNB_PCAP:-0}"
exec sudo -E env P5G_GNB_TRACE_DIR="$TRACE_DIR" P5G_GNB_TRACE_FLUSH_MS="${P5G_GNB_TRACE_FLUSH_MS:-500}" \
  /usr/bin/stdbuf -oL "$GNB" -c "$CFG" log --filename "$TRACE_DIR/gnb.log" "${PCAP_ARGS[@]}" "$@"   # stdbuf: line-buffered stdout when redirected to a file

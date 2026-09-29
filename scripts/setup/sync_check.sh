#!/usr/bin/env bash
# Go / no-go for clock sync before a run:  scripts/setup/sync_check.sh [max_offset_ms=1] [server=192.168.77.1]
# Prints chrony's current offset / RMS offset, the source jitter and the LAN RTT to the server (offset error is
# bounded by RTT/2 on a symmetric LAN). Exit 1 if the RMS offset exceeds the threshold.
set -uo pipefail
MAX="${1:-1}"; SERVER="${2:-192.168.77.1}"
T="$(chronyc tracking 2>/dev/null)" || { echo "chrony not running"; exit 1; }
echo "$T" | grep -E "Reference ID|System time|RMS offset|Leap"
chronyc sources -v 2>/dev/null | tail -2
RTT="$(ping -c 5 -i 0.2 -q "$SERVER" 2>/dev/null | awk -F/ '/rtt/{print $5}')"; [ -n "$RTT" ] && echo "LAN rtt avg ${RTT} ms -> offset error bound ~$(awk -v r="$RTT" 'BEGIN{printf "%.3f", r/2}') ms"
RMS_MS="$(echo "$T" | awk '/RMS offset/{printf "%.3f", $4*1000}')"
awk -v r="$RMS_MS" -v m="$MAX" 'BEGIN{ if (r+0 > m+0) { print "NO-GO: RMS offset " r " ms > " m " ms"; exit 1 } else { print "GO: RMS offset " r " ms <= " m " ms" } }'

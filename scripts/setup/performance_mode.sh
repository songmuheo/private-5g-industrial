#!/usr/bin/env bash
# Put this host into the srsRAN-recommended performance state (gNB PC and UE laptops), non-interactively.
#
#   scripts/setup/performance_mode.sh          apply (needs sudo) and print the resulting state
#   scripts/setup/performance_mode.sh --check  print the state only; exit 1 if any CPU is not on "performance"
#
# Based on: third_party/srsRAN_Project/scripts/srsran_performance (same three settings, same values; that script
# asks Y/n for each, which an orchestrated run cannot answer):
#   1. CPU scaling governor = performance on every CPU (srsRAN warns "CPUn scaling governor is not set to
#      performance" at gNB start; frequency ramps add processing jitter on the RAN and media threads)
#   2. DRM KMS polling off (/sys/module/drm_kms_helper/parameters/poll = N; periodic display polling wakes CPUs)
#   3. socket buffer limits/defaults >= 32 MiB (net.core.{w,r}mem_{max,default}; raised only, never lowered)
# None of this survives a reboot; run_gnb_core.sh applies it on the gNB PC, run_experiment.sh's preflight reports
# each laptop's governor (gov=).
set -euo pipefail
BUF=33554432   # 32 MiB, srsran_performance set_network_buffers
state() {
  local govs; govs="$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | awk '{printf "%s%sx%s", (n++?",":""), $1, $2}')"
  local kms; kms="$(cat /sys/module/drm_kms_helper/parameters/poll 2>/dev/null || echo n/a)"
  echo "governor=${govs:-n/a} kms_poll=$kms rmem_max=$(sysctl -n net.core.rmem_max) wmem_max=$(sysctl -n net.core.wmem_max)"
}
if [ "${1:-}" = "--check" ]; then
  state; ! grep -qv '^performance$' /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null; exit $?
fi
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null
[ -e /sys/module/drm_kms_helper/parameters/poll ] && echo N | sudo tee /sys/module/drm_kms_helper/parameters/poll >/dev/null
for k in wmem_max rmem_max wmem_default rmem_default; do   # raise only: never lower a value set larger elsewhere (e.g. UHD's 50 MB rmem_max)
  [ "$(sysctl -n "net.core.$k")" -ge "$BUF" ] || sudo sysctl -q -w "net.core.$k=$BUF"; done
state

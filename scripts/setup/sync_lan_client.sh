#!/usr/bin/env bash
# UE laptop: join the clock-sync LAN with a static address and sync chrony to the gNB PC only.
#   scripts/setup/sync_lan_client.sh <K>                          -> auto-detects the wired port on the switch; laptop K gets 192.168.77.1K
#   scripts/setup/sync_lan_client.sh <iface> <K> [server]         -> explicit interface
# K = camera id (the laptop that runs --stream-id camK). Installs chrony if missing (replaces systemd-timesyncd).
# The connection never becomes the default route: the phone tether keeps carrying the experiment traffic.
set -euo pipefail
# ---- ADDED: auto-detect the wired port on the switch when iface is "auto" or omitted ----
# = an Ethernet interface with link (LOWER_UP) that is not the phone tether (the interface of the default route),
#   not docker/bridge/wireless/loopback. Exactly one match is required; otherwise the candidates are listed.
detect_iface() {
  local def cands=()
  def="$(ip -4 route show default | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1); exit}')"
  while read -r name state _; do
    case "$name" in lo|docker*|br-*|veth*|virbr*|wl*|ww*|tun*|tap*) continue;; esac
    [ "$state" = "UP" ] || continue
    [ "$name" = "$def" ] && continue
    [ "$(cat /sys/class/net/$name/carrier 2>/dev/null)" = "1" ] || continue
    cands+=("$name")
  done < <(ip -br link | sed 's/@[^ ]*//')
  if [ "${#cands[@]}" -eq 1 ]; then echo "${cands[0]}"; return 0; fi
  echo "cannot auto-detect the sync-LAN interface (candidates: ${cands[*]:-none}; default route on: ${def:-none}). Pass it explicitly." >&2; return 1
}
if [ "${1:-auto}" = "auto" ] || [[ "${1:-}" =~ ^[0-9]$ ]]; then
  # forms: `sync_lan_client.sh K`, `sync_lan_client.sh auto K [server]`
  if [[ "${1:-}" =~ ^[0-9]$ ]]; then set -- auto "$@"; fi
  IFACE="$(detect_iface)"; echo "[sync-client] detected sync-LAN interface: $IFACE"; shift
else IFACE="$1"; shift; fi
K="${1:?K (camera id 0..9)}"; SERVER="${2:-192.168.77.1}"
# ---- END ADDED ----------------
IP="${SERVER%.*}.1${K}/24"; CON="p5g-sync"
echo "[sync-client] $IFACE <- $IP, NTP server $SERVER"
if ! nmcli -t -f NAME con show | grep -qx "$CON"; then
  sudo nmcli con add type ethernet ifname "$IFACE" con-name "$CON" ipv4.method manual ipv4.addresses "$IP" ipv4.never-default yes ipv6.method disabled connection.autoconnect yes
else
  sudo nmcli con mod "$CON" connection.interface-name "$IFACE" ipv4.method manual ipv4.addresses "$IP" ipv4.never-default yes ipv6.method disabled
fi
sudo nmcli con up "$CON" >/dev/null
command -v chronyd >/dev/null || sudo apt-get install -y chrony
# One reference only (the gNB PC): disable the distro pool lines so the laptops do not straddle two references.
sudo sed -i -E 's/^(pool|server) /# p5g: &/' /etc/chrony/chrony.conf
sudo tee /etc/chrony/conf.d/p5g-sync-client.conf >/dev/null <<CONF
# gNB PC over the sync LAN: 1-4 s polling, interleaved mode
server $SERVER iburst minpoll 0 maxpoll 2 xleave
# step only right after start; slew afterwards (never during a run)
makestep 1 3
maxdistance 0.05
CONF
sudo systemctl restart chrony
echo "[sync-client] default route (must stay on the tether):"; ip route show default
sleep 15; chronyc tracking | sed 's/^/[sync-client] /'; chronyc sources -v | tail -3

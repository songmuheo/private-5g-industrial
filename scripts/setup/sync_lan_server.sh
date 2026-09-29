#!/usr/bin/env bash
# gNB PC: bring up the clock-sync LAN on a spare wired port and serve NTP (chrony) on it.
#   scripts/setup/sync_lan_server.sh [iface=enp4s0] [ip=192.168.77.1/24]
# Topology: gNB PC <iface> -- unmanaged switch (e.g. ipTIME H6008) -- UE laptops (USB/onboard Ethernet).
# No DHCP, no gateway: static addresses only, so the laptops' default route stays on the phone tether.
# Also installs firewall rules on <iface> that admit only NTP and SSH: the receiver's ICE would otherwise
# find this LAN (it gathers host candidates on every interface) and move the media off the 5G path.
# Re-run after a reboot for the iptables part (rules are not persisted); nmcli and chrony settings persist.
set -euo pipefail
IFACE="${1:-enp4s0}"; CIDR="${2:-192.168.77.1/24}"; NET="${CIDR%.*}.0/24"; IP="${CIDR%/*}"
CON="p5g-sync"
echo "[sync-server] $IFACE <- $CIDR (connection $CON), NTP for $NET"
if ! nmcli -t -f NAME con show | grep -qx "$CON"; then
  sudo nmcli con add type ethernet ifname "$IFACE" con-name "$CON" ipv4.method manual ipv4.addresses "$CIDR" ipv4.never-default yes ipv6.method disabled connection.autoconnect yes
else
  sudo nmcli con mod "$CON" ipv4.method manual ipv4.addresses "$CIDR" ipv4.never-default yes ipv6.method disabled
fi
sudo nmcli con up "$CON" >/dev/null
command -v chronyd >/dev/null || sudo apt-get install -y chrony
sudo tee /etc/chrony/conf.d/p5g-sync-server.conf >/dev/null <<CONF
# p5g clock-sync LAN server (gNB PC). Clients: UE laptops on $NET.
# (chrony.conf accepts comments only on their own lines)
bindaddress $IP
allow $NET
# keep serving the lab even if the upstream (pool / campus NTP) is unreachable
local stratum 8
CONF
sudo systemctl restart chrony
# Firewall: on the sync LAN accept only NTP (UDP 123) and SSH (TCP 22); drop the rest (ICE/STUN/RTP/signaling).
sudo iptables -C INPUT -i "$IFACE" -p udp --dport 123 -j ACCEPT 2>/dev/null || sudo iptables -A INPUT -i "$IFACE" -p udp --dport 123 -j ACCEPT
sudo iptables -C INPUT -i "$IFACE" -p tcp --dport 22 -j ACCEPT 2>/dev/null  || sudo iptables -A INPUT -i "$IFACE" -p tcp --dport 22 -j ACCEPT
sudo iptables -C INPUT -i "$IFACE" -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT 2>/dev/null || sudo iptables -A INPUT -i "$IFACE" -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
sudo iptables -C INPUT -i "$IFACE" -j DROP 2>/dev/null || sudo iptables -A INPUT -i "$IFACE" -j DROP
sleep 2; chronyc tracking | sed 's/^/[sync-server] /'
echo "[sync-server] laptops: scripts/setup/sync_lan_client.sh <iface> <K>   (-> ${CIDR%.*}.1K, server $IP)"

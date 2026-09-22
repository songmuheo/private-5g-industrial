#!/usr/bin/env bash
# Start (or stop) the Open5GS core and set up host routing to the UE subnet.
#   scripts/run/start_core.sh        -> up, wait for healthy, add route 10.45.0.0/16 via 10.53.1.2
#   scripts/run/start_core.sh down   -> down (route and NAT rule removed)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMPOSE=(docker compose -f "$ROOT/ran/core/docker-compose.yml")

# Internet uplink for the UEs: the UPF in the srsRAN recipe hands UE traffic to this host, which must
# forward and masquerade the UE subnet on its default-route NIC. Without it phones and tethered laptops
# only reach this host (enough for a local receiver) and Android's connectivity check fails.
WAN_IF="$(ip -4 route show default | awk '{print $5; exit}')"
NAT_RULE=(-s 10.45.0.0/16 -o "$WAN_IF" -j MASQUERADE)

if [ "${1:-up}" = "down" ]; then
  "${COMPOSE[@]}" down
  sudo ip route del 10.45.0.0/16 via 10.53.1.2 2>/dev/null || true
  sudo iptables -t nat -D POSTROUTING "${NAT_RULE[@]}" 2>/dev/null || true
  exit 0
fi

"${COMPOSE[@]}" up -d
echo "[start_core] waiting for Open5GS to become healthy..."
for i in $(seq 1 60); do
  st="$(docker inspect -f '{{.State.Health.Status}}' p5g_open5gs 2>/dev/null || echo starting)"
  [ "$st" = "healthy" ] && break
  sleep 2
done
[ "$st" = "healthy" ] || { echo "[start_core] core not healthy: $st" >&2; docker logs --tail 40 p5g_open5gs; exit 1; }

# Host route to UE addresses through the core (same as the srsRAN docker README). Lets the
# receiver on this host reach UE IPs directly and lets tcpdump on ogstun-side traffic work.
if ! ip route show 10.45.0.0/16 | grep -q via; then
  sudo ip route add 10.45.0.0/16 via 10.53.1.2
fi
sudo sysctl -q -w net.ipv4.ip_forward=1
sudo iptables -t nat -C POSTROUTING "${NAT_RULE[@]}" 2>/dev/null || sudo iptables -t nat -A POSTROUTING "${NAT_RULE[@]}"
# Extra DNNs the phones may request besides srsapn/ims (space separated; default: the Pixels' "oai" APN).
for dnn in ${P5G_UE_DNNS:-oai}; do "$(dirname "$0")/core_add_dnn.sh" "$dnn"; done
echo "[start_core] Open5GS up at 10.53.1.2 (AMF N2 38412, UPF N3 2152); UE pool 10.45.0.0/16 routed"

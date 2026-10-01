#!/usr/bin/env bash
# Control of the Pixel tethered to THIS laptop over adb (run on the laptop; ffmpeg/run_session.sh calls it over SSH).
#
#   ffmpeg/scripts/phone.sh check                 adb authorised, unlocked, not in battery saver -> "ready ..." (exit 1 if not)
#   ffmpeg/scripts/phone.sh airplane on|off       airplane mode (cmd connectivity airplane-mode, Android 11+)
#   ffmpeg/scripts/phone.sh wait-attach SECS      wait until the phone holds a UE address of the core's pool (10.45.x)
#   ffmpeg/scripts/phone.sh tether on|off         USB tethering, then verify on the laptop (interface + IPv4) -> "tether=<iface>"
#   ffmpeg/scripts/phone.sh path                  ping the 5G core (10.53.1.1) through the tether interface -> "path=ok ..."
#
# Android 17 has no shell command for USB tethering (`cmd tethering`: no shell implementation; `svc usb setFunctions rndis`
# does not start the tethering service and makes the phone re-enumerate). So `tether` opens Settings > Hotspot &
# tethering (android.settings.TETHER_SETTINGS), reads the UI tree (uiautomator), taps the switch on the "USB tethering"
# row only if it is not already in the wanted state, and accepts the result only when the laptop sees the interface.
# Every tethering change re-enumerates USB: a phone whose "Always allow from this computer" was not ticked becomes
# "unauthorized" and needs a tap on its screen -> reported as such (exit 3).
set -uo pipefail
ADB="$(command -v adb || echo ~/.local/bin/adb)"
CORE_IP="${P5G_CORE_IP:-10.53.1.1}"; UE_POOL_RE="${P5G_UE_POOL_RE:-^10\.45\.}"
SYNC_RE='^192\.168\.77\.'
sh_() { "$ADB" shell "$@" 2>/dev/null | tr -d '\r'; }
adb_state() { "$ADB" devices 2>/dev/null | awk 'NR > 1 && NF >= 2 {print $2}' | head -1; }
need_adb() {
  local s; s="$(adb_state)"
  case "$s" in
    device) return 0;;
    unauthorized) echo "adb=unauthorized: tap 'Allow' on the phone and tick 'Always allow from this computer'"; exit 3;;
    "") echo "adb=none: no phone with USB debugging on this laptop"; exit 3;;
    *) echo "adb=$s"; exit 3;;
  esac
}
# laptop interface of the phone's USB tethering: an interface with an IPv4 address that is not lo, Wi-Fi or the sync LAN
tether_iface() {
  ip -o -4 addr show 2>/dev/null | awk '{print $2, $4}' | while read -r dev addr; do
    case "$dev" in lo|wl*) continue;; esac
    [[ "${addr%/*}" =~ $SYNC_RE ]] && continue
    [[ "$dev" == enx* || "$dev" == usb* || "$dev" == rndis* ]] && { echo "$dev"; break; }
  done
}
ui_switch() {   # prints "<checked> <x> <y>" of the switch on the USB tethering row of the current screen
  sh_ uiautomator dump /sdcard/p5g_ui.xml >/dev/null
  "$ADB" exec-out cat /sdcard/p5g_ui.xml 2>/dev/null | python3 -c '
import re, sys, xml.etree.ElementTree as ET
root = ET.fromstring(sys.stdin.read())
b = lambda n: tuple(map(int, re.findall(r"-?\d+", n.get("bounds", "[0,0][0,0]"))))
title = next((n for n in root.iter("node") if n.get("text", "").strip().lower() == "usb tethering"), None)
if title is None: sys.exit(2)
x1, y1, x2, y2 = b(title); cy = (y1 + y2) // 2
sw = [n for n in root.iter("node") if n.get("checkable") == "true" and b(n)[1] <= cy <= b(n)[3]]
if not sw: sys.exit(3)
s = sw[0]; X1, Y1, X2, Y2 = b(s)
print(s.get("checked"), (X1 + X2) // 2, (Y1 + Y2) // 2, s.get("enabled"))'
}

cmd="${1:-check}"
case "$cmd" in
  check)
    need_adb
    model="$(sh_ getprop ro.product.model)"; kg="$(sh_ dumpsys window | grep -m1 -oE 'isKeyguardShowing=[a-z]+' | cut -d= -f2)"
    saver="$(sh_ settings get global low_power)"; air="$(sh_ settings get global airplane_mode_on)"
    bat="$(sh_ dumpsys battery | grep -m1 ' level:' | tr -dc 0-9)"; stay="$(sh_ settings get global stay_on_while_plugged_in)"
    echo "model=${model// /_} keyguard=$kg battery_saver=$saver airplane=$air battery=${bat}% stay_awake=$stay"
    [ "$kg" = "true" ] && { echo "NOT READY: the phone's screen is locked (set Screen lock: None, or unlock it)"; exit 1; }
    [ "$saver" = "1" ] && sh_ settings put global low_power 0
    [ "$stay" = "7" ] || sh_ settings put global stay_on_while_plugged_in 7
    echo "ready"; exit 0;;
  airplane)
    need_adb; want="${2:?on|off}"
    sh_ cmd connectivity airplane-mode "$([ "$want" = on ] && echo enable || echo disable)" >/dev/null
    sleep 1; echo "airplane=$(sh_ settings get global airplane_mode_on)"; exit 0;;
  wait-attach)
    need_adb; t="${2:-60}"; end=$(( $(date +%s) + t ))
    while [ "$(date +%s)" -lt "$end" ]; do
      ip_="$(sh_ ip -4 -o addr show | awk '{print $4}' | cut -d/ -f1 | grep -E "$UE_POOL_RE" | head -1)"
      [ -n "$ip_" ] && { echo "attached ue_ip=$ip_"; exit 0; }
      sleep 2
    done
    echo "NOT ATTACHED after ${t}s (no ${UE_POOL_RE} address on the phone)"; exit 1;;
  tether)
    need_adb; want="${2:?on|off}"; wantc=$([ "$want" = on ] && echo true || echo false)
    cur="$(tether_iface)"
    if [ "$want" = on ] && [ -n "$cur" ]; then echo "tether=$cur (already on)"; exit 0; fi
    if [ "$want" = off ] && [ -z "$cur" ]; then echo "tether=off (already off)"; exit 0; fi
    sh_ input keyevent KEYCODE_WAKEUP >/dev/null; sh_ wm dismiss-keyguard >/dev/null
    sh_ am start -a android.settings.TETHER_SETTINGS >/dev/null; sleep 3
    sw="$(ui_switch)" || { echo "FAILED: 'USB tethering' row not found on the Settings screen"; sh_ input keyevent KEYCODE_HOME >/dev/null; exit 1; }
    read -r checked x y enabled <<<"$sw"
    [ "$enabled" = "true" ] || { echo "FAILED: the USB tethering switch is disabled on the phone"; sh_ input keyevent KEYCODE_HOME >/dev/null; exit 1; }
    [ "$checked" = "$wantc" ] || sh_ input tap "$x" "$y" >/dev/null
    # verify on the laptop; USB re-enumerates, adb may need a moment (or re-authorisation)
    for i in $(seq 1 20); do
      sleep 1; cur="$(tether_iface)"
      if [ "$want" = on ] && [ -n "$cur" ]; then break; fi
      if [ "$want" = off ] && [ -z "$cur" ]; then break; fi
    done
    for i in $(seq 1 10); do [ "$(adb_state)" = device ] && break; sleep 1; done
    [ "$(adb_state)" = device ] && sh_ input keyevent KEYCODE_HOME >/dev/null
    [ "$(adb_state)" = unauthorized ] && echo "WARNING: adb became unauthorized after the USB change (tick 'Always allow' on the phone)"
    if [ "$want" = on ]; then [ -n "$cur" ] && { echo "tether=$cur $(ip -br -4 addr show "$cur" | awk '{print $3}')"; exit 0; }; echo "FAILED: no tethering interface appeared on the laptop"; exit 1
    else [ -z "$cur" ] && { echo "tether=off"; exit 0; }; echo "FAILED: tethering interface $cur still present"; exit 1; fi;;
  path)
    dev="$(tether_iface)"; [ -n "$dev" ] || { echo "path=none (no tethering interface)"; exit 1; }
    via="$(ip route get "$CORE_IP" 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p')"
    [ "$via" = "$dev" ] || { echo "path=wrong: $CORE_IP routed via ${via:-nothing}, tethering is $dev (Wi-Fi on? sync LAN?)"; exit 1; }
    out="$(ping -n -c 3 -W 2 "$CORE_IP" 2>/dev/null | tail -1)"
    [ -n "$out" ] && echo "$out" | grep -q "/" && { echo "path=ok via $dev rtt ${out#*= }"; exit 0; }
    echo "path=FAILED: $CORE_IP not reachable through $dev (UE not attached / no PDU session?)"; exit 1;;
  *) echo "usage: phone.sh check | airplane on|off | wait-attach SECS | tether on|off | path" >&2; exit 2;;
esac

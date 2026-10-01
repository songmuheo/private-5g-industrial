#!/usr/bin/env bash
# gNB PC: run one multi-UE experiment from a scenario file, end to end.
#
#   ./run_experiment.sh experiments/<scenario>.json
#
# 1. Uses the live run directory of ./run_gnb_core.sh (results/CURRENT) or creates results/<ts>-<name>;
#    copies the scenario to <run>/scenario.json and writes <run>/experiment.json (resolved commands, start
#    time, host status, git HEAD) so the receiver side holds the full configuration of the run.
# 2. Starts the control server + one video_receiver per camera (run_receiver.sh, non-interactive).
# 3. Preflight on every camera host over the sync LAN: repo HEAD, video asset, chrony offset.
# 4. Picks T = now + start_delay_s and launches every sender with its own resolution / fps / bitrate and
#    --start-at T (each laptop waits on its chrony-synced clock), all in parallel; camera failures are recorded,
#    the others continue.
# 5. When the senders end, pulls their traces (results/*-sender-camK/app) into <run>/senders/camK/, stops the
#    receivers (traces flush), runs verify_run.py and exp_run_report.py.
#
# Scenario (JSON): see experiments/5ue-720p30.json. "hosts.mode" = "ssh" (laptops; user@ip_pattern with {K},
# repo dir under the home) or "local" (smoke test on this PC). Per camera, "host" and "repo" override the pattern.
set -euo pipefail
# Layout: this script lives in ffmpeg/ (SMEC-style transport tree). results/, analysis/, video/assets and
# scripts/setup are shared at the repo root (working directory); receivers/senders come from ffmpeg/.
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(cd "$TREE/.." && pwd)"; cd "$ROOT"
SCEN="${1:?usage: run_experiment.sh ffmpeg/experiments/<scenario>.json [--rotate R]}"; shift
[ -f "$SCEN" ] || { echo "no such scenario: $SCEN" >&2; exit 1; }
# --rotate R: camK (its profile, groups, stream id, receiver) runs on the host of cam((K+R) mod N). Same profiles, other
# phones: separates phone/placement effects from profile effects (the phones' deployment is fixed). 0 = as written.
ROTATE=0
while [ $# -gt 0 ]; do case "$1" in --rotate) ROTATE="$2"; shift 2;; *) echo "unknown arg $1" >&2; exit 1;; esac; done
[[ "$ROTATE" =~ ^[0-9]+$ ]] || { echo "--rotate needs a non-negative integer" >&2; exit 1; }
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3

# ---- scenario -> shell (python does the JSON; jq is not assumed) ----
eval "$("$PY" - "$SCEN" <<'PYEOF'
import json, sys, shlex
s = json.load(open(sys.argv[1]))
q = shlex.quote
if s.get('tree', 'ffmpeg') != 'ffmpeg': sys.exit(f"ABORT: scenario is for the {s['tree']} tree; run {s['tree']}/run_experiment.sh (this is ffmpeg/)")
print(f"NAME={q(s['name'])}; DURATION={int(s.get('duration_s', 300))}; START_DELAY={int(s.get('start_delay_s', 20))}")
print(f"RELAY_HOST={q(s.get('relay_host', '10.53.1.1'))}; REQUIRE_GNB={'1' if s.get('require_gnb', True) else '0'}")
h = s.get('hosts', {}); print(f"HOST_MODE={q(h.get('mode', 'ssh'))}; HOST_USER={q(h.get('user', 'songmu'))}; HOST_PATTERN={q(h.get('ip_pattern', '192.168.77.1{K}'))}; HOST_REPO={q(h.get('repo', 'private-5g-industrial'))}")
sy = s.get('sync', {}); print(f"SYNC_MODE={q(str(sy.get('mode', 'auto')))}; SYNC_MAX_MS={q(str(sy.get('max_ms', 1)))}")
r = s.get('receiver', {}); print(f"RECV_EXTRA={q(' '.join(r.get('extra_args', [])))}")
cams = s['cams']; print(f"CAMS=({' '.join(q(c) for c in cams)})")
for c, v in cams.items():
    if v.get('host'): print(f"CAM_HOST_{c}={q(v['host'])}")
    if v.get('repo'): print(f"CAM_REPO_{c}={q(v['repo'])}")
    # pre-encoded rungs: cams.camK.source = name prefix in video/assets (e.g. mot17-02_1280x720_30), rungs = [kbps...],
    # kbps = the rung to start with. Missing source -> run_sender.sh picks the camera's Kendo view / MOT17 rungs.
    args = [f"--fps {int(v.get('fps',30))}", f"--bitrate-kbps {int(v.get('kbps', 2500))}"]
    if int(v.get('phase_slots', 0)): args.append(f"--phase-slots {int(v['phase_slots'])}")   # IDR staggering: source index offset of this camera
    if v.get('source'):
        rungs = v.get('rungs') or [int(v.get('kbps', 2500))]
        src = v['source']
        if 'idr_origin' in v and int(v.get('phase_slots', 0)):
            sys.exit(f"ABORT: {c}: idr_origin and phase_slots together break the frame-number alignment (content = (slot+phase) mod N); use one")
        if 'idr_origin' in v:   # per-camera rotated ladder (prepare_video.sh --source mot17-03): <seq>-o<P>_<WxH>_<fps>; same frame numbers, IDRs at P+GOP*m
            name, rest = src.split('_', 1); src = f"{name}-o{int(v['idr_origin'])}_{rest}"
            args.append(f"--content-origin {int(v['idr_origin'])}")
        args.append("--source " + ",".join(f"video/assets/{src}_{int(k)}k.h264@{int(k)}" for k in rungs))
    args += v.get('extra_args', [])
    print(f"CAM_ARGS_{c}={q(' '.join(args))}")
PYEOF
)"
[ -n "${NAME:-}" ] || exit 1   # the scenario parser aborted (wrong tree / bad JSON)
N=${#CAMS[@]}
camK() { echo "${1//[!0-9]/}"; }
base_host() { local v="CAM_HOST_$1"; echo "${!v:-${HOST_PATTERN//\{K\}/$(camK "$1")}}"; }    # cams.camK.host overrides the pattern
base_repo() { local v="CAM_REPO_$1"; echo "${!v:-$HOST_REPO}"; }                               # cams.camK.repo overrides hosts.repo
# rotation over the cameras in NUMERIC id order (cam0, cam1, ..), independent of the key order in the JSON
mapfile -t SORTED_CAMS < <(printf '%s\n' "${CAMS[@]}" | sort -t m -k2 -n)
cam_index() { local i; for i in "${!SORTED_CAMS[@]}"; do [ "${SORTED_CAMS[$i]}" = "$1" ] && { echo "$i"; return; }; done; }
host_cam() { echo "${SORTED_CAMS[$(( ($(cam_index "$1") + ROTATE) % N ))]}"; }                # whose host this cam runs on
host_of() { base_host "$(host_cam "$1")"; }
repo_of() { base_repo "$(host_cam "$1")"; }
# run a command on a camera host (ssh) or locally
on_host() { local cam="$1"; shift
  if [ "$HOST_MODE" = "local" ]; then bash -lc "cd '$TREE' && $*"
  else ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST_USER@$(host_of "$cam")" "cd ~/$(repo_of "$cam")/ffmpeg && $*"; fi; }

# ---- run directory ----
if [ "$REQUIRE_GNB" = 1 ]; then pgrep -x gnb >/dev/null || { echo "[exp] gNB is not running: start ./run_gnb_core.sh first (or set require_gnb=false for a local smoke test)" >&2; exit 1; }; fi
# join the live gNB run only for a RAN experiment; a loopback smoke test (require_gnb=false) always gets its own directory
# Inside a live gNB session (results/CURRENT, ./run_gnb_core.sh) every experiment gets its own sub-run
# results/<session>/runs/<ts>-<name>-r<R>/ with gnb/ and core/ linked to the session's (one gNB, many experiments; each
# run's window is its experiment.json start time + duration).
TAG="$(date +%Y%m%d-%H%M%S)-$NAME-r$ROTATE"
if [ "$REQUIRE_GNB" = 1 ] && [ -L results/CURRENT ] && pgrep -x gnb >/dev/null; then
  SESSION="results/$(readlink results/CURRENT)"; RD="$SESSION/runs/$TAG"; mkdir -p "$RD"; ln -sfn ../../gnb "$RD/gnb"; ln -sfn ../../core "$RD/core"
else RD="results/$TAG"; fi
mkdir -p "$RD/app" "$RD/senders"; cp "$SCEN" "$RD/scenario.json"
LOG="$RD/experiment.log"; log() { echo "[exp $(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
log "scenario $NAME: $N cams, ${DURATION}s, start in ${START_DELAY}s, hosts=$HOST_MODE, relay=$RELAY_HOST, rotate=$ROTATE -> $RD"
[ "$ROTATE" = 0 ] || log "rotation $ROTATE: $(for c in "${CAMS[@]}"; do printf '%s->%s ' "$c" "$(host_cam "$c")"; done)(camK runs on that cam's host)"

# ---- receivers ----
# leftovers of an earlier run (receivers, control server on the port) would mix into this run: refuse to start
PORT="${P5G_CONTROL_PORT:-8765}"
if pgrep -x video_receiver >/dev/null || ss -ltn 2>/dev/null | grep -q ":$PORT "; then   # -x: process name, not command-line text
  log "ABORT: receivers or a server on :$PORT are still running from an earlier run:"; { pgrep -a -x video_receiver; ss -ltnp 2>/dev/null | grep ":$PORT "; } | cut -c1-120 | tee -a "$LOG"
  log "stop them first: pkill -f ffmpeg/build/apps/video_receiver; pkill -f ffmpeg/apps/control/control_server.py"; exit 1
fi
P5G_RECEIVER_NOTAIL=1 "$TREE/run_receiver.sh" "$RD" -n "$N" $RECV_EXTRA > "$RD/app/run_receiver.log" 2>&1 &
RECV_PID=$!; sleep 2
kill -0 "$RECV_PID" 2>/dev/null || { cat "$RD/app/run_receiver.log"; echo "[exp] receivers failed to start" >&2; exit 1; }
log "receivers up (pid $RECV_PID): $(grep -c 'pid=' "$RD/app/run_receiver.log") of $N"
# stop: TERM run_receiver.sh (its EXIT trap INTs the receivers so traces flush, then stops the control server); bounded
# wait; then, as a fallback, stop whatever of THIS run is still alive from receiver.pids (INT, then KILL after 10 s)
declare -A LPID; LAUNCHED=(); FINISHED=0
# The senders of THIS launch carry P5G_RUN_ID=<RUN_ID> in their environment (run_sender.sh is started with it): only those
# are signalled, never another experiment's or another tree's video_sender on the same laptop.
owned_stop_cmd() {   # remote shell snippet: INT this run's senders, wait up to 10 s, print how many are left
  local id; id="$(printf %q "P5G_RUN_ID=$RUN_ID")"
  echo "mine() { for p in \$(pgrep -x video_sender); do tr '\\0' '\\n' </proc/\$p/environ 2>/dev/null | grep -qxF $id && echo \$p; done; };" \
       "for p in \$(mine); do kill -INT \$p; done; for i in \$(seq 1 50); do [ -z \"\$(mine)\" ] && break; sleep 0.2; done; echo remaining=\$(mine | wc -l)"
}
stop_senders() {   # aborted run: stop this run's senders on every host they were launched on (launcher alive or not)
  [ "${#LAUNCHED[@]}" -gt 0 ] || return 0
  log "aborting: stopping this run's senders on their hosts: ${LAUNCHED[*]}"
  local c; declare -A SP
  for c in "${LAUNCHED[@]}"; do on_host "$c" "$(owned_stop_cmd)" > "$RD/senders/$c.stop.log" 2>&1 & SP[$c]=$!; done
  for c in "${LAUNCHED[@]}"; do wait "${SP[$c]}" 2>/dev/null
    grep -q "remaining=0" "$RD/senders/$c.stop.log" && log "$c: sender stopped" || log "WARNING: $c: sender stop not confirmed ($(tail -1 "$RD/senders/$c.stop.log"))"; done
  for c in "${LAUNCHED[@]}"; do kill -0 "${LPID[$c]}" 2>/dev/null && kill -TERM "${LPID[$c]}" 2>/dev/null; done
}
collect_senders() {   # this launch's sender directories only (results/<RUN_ID>-sender-<cam>); missing/failed -> MISSING
  local c src
  for c in "${CAMS[@]}"; do
    case "${STATUS[$c]:-}" in UNREACHABLE*) log "$c: unreachable, no traces"; MISSING="${MISSING:-} $c(unreachable)"; continue;; esac
    mkdir -p "$RD/senders/$c"
    src="results/$RUN_ID-sender-$c"   # (results/ is at the repo root; on_host cd's into ffmpeg/)
    if [ "$HOST_MODE" = "local" ]; then
      if [ -d "$src/app" ] && cp -r "$src/app" "$RD/senders/$c/"; then log "collected $c from $src"; else log "$c: no sender traces ($src missing or copy failed)"; MISSING="${MISSING:-} $c"; fi
    else
      if rsync -aq "$HOST_USER@$(host_of "$c"):~/$(repo_of "$c")/$src/app/" "$RD/senders/$c/app/" 2>/dev/null; then log "collected $c from $(host_of "$c"):$src"
      else log "$c: no sender traces on $(host_of "$c") ($src missing)"; MISSING="${MISSING:-} $c"; fi
    fi
    if [ -n "${FAILED[$c]:-}" ]; then MISSING="${MISSING:-} $c(failed)"; fi
  done
  return 0   # (a false test as the last command would make the function "fail" under set -e)
}
finalize_abort() {   # EXIT before the normal end: stop + collect what exists, mark the run aborted
  [ "$FINISHED" = 1 ] && return 0
  [ "${#LAUNCHED[@]}" -gt 0 ] || return 0
  stop_senders; collect_senders
  mkdir -p "$RD/analysis"; echo "INVALID: run aborted before its end (senders stopped, traces collected as far as they exist)" >> "$RD/analysis/INVALID"
  log "RESULT INVALID: aborted"
}
cleanup() { set +e; finalize_abort; log "stopping receivers"; kill -TERM "$RECV_PID" 2>/dev/null
  for i in $(seq 1 150); do kill -0 "$RECV_PID" 2>/dev/null || break; sleep 0.1; done
  local left=""; [ -f "$RD/app/receiver.pids" ] && left="$(sed -n 's/^[a-z]*=//p' "$RD/app/receiver.pids" | tr ' ' '\n' | while read -r p; do [ -n "$p" ] && kill -0 "$p" 2>/dev/null && echo "$p"; done)"
  if [ -n "$left" ] || kill -0 "$RECV_PID" 2>/dev/null; then
    log "WARNING: receiver processes still alive after 15 s ($left) -> INT, then KILL"; kill -INT $left 2>/dev/null; kill -TERM "$RECV_PID" 2>/dev/null
    for i in $(seq 1 100); do alive=0; for p in $left; do kill -0 "$p" 2>/dev/null && alive=1; done; [ $alive = 0 ] && break; sleep 0.1; done
    kill -KILL $left "$RECV_PID" 2>/dev/null
  fi
  wait "$RECV_PID" 2>/dev/null; }
trap cleanup EXIT

# ---- preflight ----
# Sync-LAN guard: the media destination is fixed here (control host, no ICE), but the laptops must still not be
# able to reach anything but NTP/SSH over the sync LAN (scripts/setup/sync_lan_server.sh); same rule as webrtc/.
if [ "$HOST_MODE" = "ssh" ] && ! sudo -n iptables -S P5G_SYNC_OUT 2>/dev/null | grep -q -- "-j DROP"; then
  log "ABORT: sync-LAN firewall chains missing (run scripts/setup/sync_lan_server.sh; it is not persisted across reboots)"; exit 1
fi
declare -A STATUS
for c in "${CAMS[@]}"; do
  var="CAM_ARGS_$c"; args="${!var}"
  # every rung file of the camera (the sender loads them all), each checked against the sha256 in the manifest
  srcs="$(sed -n 's/.*--source \([^ ]*\).*/\1/p' <<<"$args" | tr ',' '\n' | sed 's/@.*//' | tr '\n' ' ')"   # repo-relative; on_host cd's into ffmpeg/ -> ../
  chk="for f in $srcs; do b=\$(basename \$f); [ -f ../\$f ] || { echo asset=MISSING:\$b; continue; }; w=\$(grep \"^file=\$b \" ../video/assets/h264_ladders.txt | sed -n 's/.* sha256=//p'); [ -n \"\$w\" ] && [ \"\$w\" = \"\$(sha256sum ../\$f | cut -c1-64)\" ] || echo asset=MISSING:\$b:sha; done; echo assets_checked=\$(echo $srcs | wc -w);"
  [ -n "$srcs" ] || chk=""
  # media route: on a laptop the route to the receiver host must leave through the phone's tethering interface — not
  # Wi-Fi (media would bypass the 5G link) and not the sync LAN (firewalled; only NTP/SSH). Reported as route_dev=.
  rchk="d=\$(ip route get $RELAY_HOST 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p'); s=\$(ip -o -4 addr show | awk '/ 192\.168\.77\./{print \$2}'); echo route_dev=\${d:-none}; case \"\$d\" in ''|wl*) echo route=BAD;; \"\$s\") echo route=BAD;; *) echo route=ok;; esac;"
  [ "$HOST_MODE" = "local" ] && rchk=""
  rchk="$rchk g=\$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | paste -sd+); echo gov=\${g:-n/a};"   # performance expected (scripts/setup/performance_mode.sh)
  out="$(on_host "$c" "git rev-parse --short HEAD 2>/dev/null; $chk $rchk chronyc tracking 2>/dev/null | awk '/RMS offset/{print \"rms_ms=\" \$4*1000}' || echo chrony=none" 2>&1 | tr '\n' ' ')" \
    && STATUS[$c]="ok: $out" || STATUS[$c]="UNREACHABLE: $out"
  log "preflight $c @ $( [ "$HOST_MODE" = local ] && echo local || host_of "$c"): ${STATUS[$c]}"
  case "${STATUS[$c]}" in *asset=MISSING*) log "ABORT: $c: pre-encoded source missing on its host (run ffmpeg/scripts/prepare_client.sh there)"; exit 1;; esac
  case "${STATUS[$c]}" in *gov=performance\ *) ;; *) log "WARNING: $c: CPU governor is not 'performance' on its host ($(grep -o 'gov=[^ ]*' <<<"${STATUS[$c]}")): run scripts/setup/performance_mode.sh there";; esac
  case "${STATUS[$c]}" in *route=BAD*) log "ABORT: $c: media to $RELAY_HOST would not go over the 5G phone ($(grep -o 'route_dev=[^ ]*' <<<"${STATUS[$c]}")): turn Wi-Fi off (nmcli radio wifi off) and check the phone's USB tethering"; exit 1;; esac
done
log "gNB PC HEAD $(git rev-parse --short HEAD)"
# ---- radio link check from the live gNB table (last ~10 s of gnb_stdout.log): CQI and power headroom per UE ----
# PHR <= 3 dB means the phone is already at maximum transmit power (no margin: RLF on any extra loss);
# CQI < 9 means the DL is marginal (feedback path, UE out-of-sync risk). Both were seen on 2026-09-29
# (PHR 0 dB, CQI 5-7) after re-cabling, versus 23 dB / 13-15 the day before with identical gNB settings.
if [ "$HOST_MODE" = "ssh" ] && [ -f "$RD/gnb/gnb_stdout.log" ]; then
  "$PY" - "$RD/gnb/gnb_stdout.log" <<'PYEOF' | tee -a "$LOG"
import re, sys, collections, statistics as st
rows = collections.defaultdict(list)
for line in open(sys.argv[1]).readlines()[-400:]:
    m = re.match(r'\s+\d+\s+([0-9a-f]{4})\s*\|\s*(\S+)\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s*\|\s*(\S+)\s+(\S+)\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S+)', line)
    if m: rows[m[1]].append(m.groups()[1:])
if not rows: print("[exp] link check: no UE rows in the gNB table yet (phones attached?)")
for rnti, v in rows.items():
    v = v[-10:]
    def med(i):
        x = [float(a[i]) for a in v if a[i] not in ('n/a', '')]; return st.median(x) if x else None
    cqi, snr, rsrp, phr = med(0), med(1), med(2), med(3)
    flag = []
    if phr is not None and phr <= 3: flag.append(f"PHR {phr:.0f} dB: phone at max power, no margin")
    if cqi is not None and cqi < 9: flag.append(f"CQI {cqi:.0f}: DL marginal")
    print(f"[exp] link check rnti 0x{rnti}: CQI {cqi} PUSCH SNR {snr} dB RSRP {rsrp} dBFS PHR {phr} dB -> {'WARNING: ' + '; '.join(flag) + ' (check antennas/cables/phone placement; see docs/NOTES.md 2026-09-29)' if flag else 'OK'}")
PYEOF
fi

# ---- launch ----
# Every sender of this launch writes to results/<RUN_ID>-sender-<cam> on its host (run_sender.sh P5G_RUN_ID), so the
# collection below takes exactly that directory — never "the newest one", which after a failed launch is a
# previous run's.
RUN_ID="$(basename "$(readlink -f "$RD")")-$(date +%H%M%S)"
T="$(awk -v n="$(date +%s.%N)" -v d="$START_DELAY" 'BEGIN{printf "%.3f", n+d}')"
log "start time T=$T ($(date -d "@$T" +%H:%M:%S.%3N))"
for c in "${CAMS[@]}"; do
  case "${STATUS[$c]}" in UNREACHABLE*) log "skip $c (unreachable)"; continue;; esac
  var="CAM_ARGS_$c"; K="$(camK "$c")"
  cmd="P5G_RUN_ID=$(printf %q "$RUN_ID") P5G_SYNC=$SYNC_MODE P5G_SYNC_MAX_MS=$SYNC_MAX_MS P5G_CONTROL_PORT=${P5G_CONTROL_PORT:-8765} ./run_sender.sh $RELAY_HOST --to recv$K --stream-id $c ${!var} --duration $DURATION --start-at $T"
  log "launch $c: $cmd"
  on_host "$c" "$cmd" > "$RD/senders/$c.launch.log" 2>&1 &
  LPID[$c]=$!; LAUNCHED+=("$c")
done
# resolved configuration record
"$PY" - "$RD" "$T" "$HOST_MODE" "$SCEN" "$ROTATE" <<'PYEOF' $(for c in "${CAMS[@]}"; do printf '%s=%s=%s ' "$c" "$(host_cam "$c")" "$([ "$HOST_MODE" = local ] && echo local || host_of "$c")"; done)
import json, sys, subprocess, datetime
rd, T, mode, scen, rot, *cams = sys.argv[1:]
m = [c.split("=") for c in cams]
rec = {"scenario": json.load(open(scen)), "start_time_epoch": float(T), "start_time_local": datetime.datetime.fromtimestamp(float(T)).isoformat(),
       "host_mode": mode, "gnb_pc_git_head": subprocess.run(["git","rev-parse","HEAD"],capture_output=True,text=True).stdout.strip(),
       "rotation": int(rot), "cam_host": {c: {"as_host_of": hc, "host": h} for c, hc, h in m},   # camK's profile ran on this host/phone
       "launch_logs": {c: f"senders/{c}.launch.log" for c, _, _ in m}}
json.dump(rec, open(f"{rd}/experiment.json", "w"), indent=2)
PYEOF

# ---- wait for the senders ----
declare -A FAILED=()
for c in "${!LPID[@]}"; do
  if wait "${LPID[$c]}"; then log "$c finished"; else log "$c FAILED (see senders/$c.launch.log)"; FAILED[$c]=1; fi
done
sleep 2

# ---- collect sender traces ----
collect_senders
FINISHED=1

# ---- stop receivers, verify, report ----
cleanup; trap - EXIT
AN="$RD/analysis"; mkdir -p "$AN"   # derived outputs (summary, report, graphs, INVALID) in the run's analysis/ subdirectory (rule 3)
if [ -n "${MISSING:-}" ]; then log "RESULT INVALID: sender(s) failed or left no traces:${MISSING}"; echo "INVALID: senders failed / traces missing:${MISSING}" >> "$AN/INVALID"; fi
"$PY" analysis/verify_run.py "$RD" 2>&1 | tail -3 | tee -a "$LOG"
log "note: gNB trace files get their footers only when ./run_gnb_core.sh is stopped; re-run: make verify RD=$RD afterwards"
"$PY" analysis/exp_run_report.py "$RD" 5 > "$AN/report.txt" 2>&1 && log "report: $AN/report.txt"
if "$PY" -c "import json,sys; sys.exit(0 if json.load(open('$RD/scenario.json')).get('groups') else 1)"; then   # fusion groups declared -> group analysis
  "$PY" analysis/fusion_report.py "$RD" > "$AN/fusion_run.log" 2>&1 && log "fusion: $AN/fusion_*.txt, graphs/fusion_*" || log "fusion report FAILED (see $AN/fusion_run.log)"
fi
sed -n '/## A\./,/## B\./p' "$AN/report.txt" | grep -E "^cam|A2|selected|^  cam" | head -20
if grep -q "WARNING: check path" "$AN/report.txt" && [ "$HOST_MODE" = "ssh" ]; then
  log "RESULT INVALID: at least one stream did not travel over the 5G link (report.txt section A2). Check the sync-LAN firewall and laptop Wi-Fi."
  echo "INVALID: media not on the 5G path (see report.txt A2)" > "$AN/INVALID"
elif [ -n "${MISSING:-}" ]; then
  log "media path check skipped: run is INVALID (missing/failed senders)"
else
  log "media path check: all streams over the 5G link"
fi
log "done: $RD"

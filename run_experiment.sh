#!/usr/bin/env bash
# gNB PC: run one multi-UE experiment from a scenario file, end to end.
#
#   ./run_experiment.sh experiments/<scenario>.json
#
# 1. Uses the live run directory of ./run_gnb_core.sh (results/CURRENT) or creates results/<ts>-<name>;
#    copies the scenario to <run>/scenario.json and writes <run>/experiment.json (resolved commands, start
#    time, host status, git HEAD) so the receiver side holds the full configuration of the run.
# 2. Starts the signaling relay + one video_receiver per camera (run_receiver.sh, non-interactive).
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
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; cd "$ROOT"
SCEN="${1:?usage: run_experiment.sh experiments/<scenario>.json}"
[ -f "$SCEN" ] || { echo "no such scenario: $SCEN" >&2; exit 1; }
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3

# ---- scenario -> shell (python does the JSON; jq is not assumed) ----
eval "$("$PY" - "$SCEN" <<'PYEOF'
import json, sys, shlex
s = json.load(open(sys.argv[1]))
q = shlex.quote
print(f"NAME={q(s['name'])}; DURATION={int(s.get('duration_s', 300))}; START_DELAY={int(s.get('start_delay_s', 20))}")
print(f"RELAY_HOST={q(s.get('relay_host', '10.53.1.1'))}; REQUIRE_GNB={'1' if s.get('require_gnb', True) else '0'}")
h = s.get('hosts', {}); print(f"HOST_MODE={q(h.get('mode', 'ssh'))}; HOST_USER={q(h.get('user', 'songmu'))}; HOST_PATTERN={q(h.get('ip_pattern', '192.168.77.1{K}'))}; HOST_REPO={q(h.get('repo', 'private-5g-industrial'))}")
sy = s.get('sync', {}); print(f"SYNC_MODE={q(str(sy.get('mode', 'auto')))}; SYNC_MAX_MS={q(str(sy.get('max_ms', 1)))}")
r = s.get('receiver', {}); print(f"RECV_EXTRA={q(' '.join(r.get('extra_args', [])))}")
cams = s['cams']; print(f"CAMS=({' '.join(q(c) for c in cams)})")
for c, v in cams.items():
    if v.get('host'): print(f"CAM_HOST_{c}={q(v['host'])}")
    if v.get('repo'): print(f"CAM_REPO_{c}={q(v['repo'])}")
    args = [f"--width {int(v.get('width',1280))}", f"--height {int(v.get('height',720))}", f"--fps {int(v.get('fps',30))}",
            f"--start-bitrate-kbps {v.get('start_kbps','auto')}"]
    if int(v.get('max_kbps', 0)) > 0: args.append(f"--max-bitrate-kbps {int(v['max_kbps'])}")
    if v.get('degradation'): args.append(f"--degradation {v['degradation']}")
    if v.get('source'): args.append(f"--yuv video/assets/{v['source']}")
    args += v.get('extra_args', [])
    print(f"CAM_ARGS_{c}={q(' '.join(args))}")
PYEOF
)"
N=${#CAMS[@]}
camK() { echo "${1//[!0-9]/}"; }
host_of() { local v="CAM_HOST_$1"; echo "${!v:-${HOST_PATTERN//\{K\}/$(camK "$1")}}"; }      # cams.camK.host overrides the pattern
repo_of() { local v="CAM_REPO_$1"; echo "${!v:-$HOST_REPO}"; }                                 # cams.camK.repo overrides hosts.repo
# run a command on a camera host (ssh) or locally
on_host() { local cam="$1"; shift
  if [ "$HOST_MODE" = "local" ]; then bash -lc "cd '$ROOT' && $*"
  else ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST_USER@$(host_of "$cam")" "cd ~/$(repo_of "$cam") && $*"; fi; }

# ---- run directory ----
if [ "$REQUIRE_GNB" = 1 ]; then pgrep -x gnb >/dev/null || { echo "[exp] gNB is not running: start ./run_gnb_core.sh first (or set require_gnb=false for a local smoke test)" >&2; exit 1; }; fi
if [ -L results/CURRENT ] && pgrep -x gnb >/dev/null; then RD="results/$(readlink results/CURRENT)"; else RD="results/$(date +%Y%m%d-%H%M%S)-$NAME"; fi
mkdir -p "$RD/app" "$RD/senders"; cp "$SCEN" "$RD/scenario.json"
LOG="$RD/experiment.log"; log() { echo "[exp $(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
log "scenario $NAME: $N cams, ${DURATION}s, start in ${START_DELAY}s, hosts=$HOST_MODE, relay=$RELAY_HOST -> $RD"

# ---- receivers ----
P5G_RECEIVER_NOTAIL=1 ./run_receiver.sh "$RD" -n "$N" $RECV_EXTRA > "$RD/app/run_receiver.log" 2>&1 &
RECV_PID=$!; sleep 2
kill -0 "$RECV_PID" 2>/dev/null || { cat "$RD/app/run_receiver.log"; echo "[exp] receivers failed to start" >&2; exit 1; }
log "receivers up (pid $RECV_PID): $(grep -c 'pid=' "$RD/app/run_receiver.log") of $N"
cleanup() { set +e; log "stopping receivers"; kill -TERM "$RECV_PID" 2>/dev/null; wait "$RECV_PID" 2>/dev/null; }   # TERM: a backgrounded shell ignores INT; its EXIT trap INTs the receivers (traces flush)
trap cleanup EXIT

# ---- preflight ----
declare -A STATUS
for c in "${CAMS[@]}"; do
  var="CAM_ARGS_$c"; args="${!var}"; src="$(sed -n 's/.*--yuv \([^ ]*\).*/\1/p' <<<"$args")"
  out="$(on_host "$c" "git rev-parse --short HEAD 2>/dev/null; [ -z '$src' ] || { [ -f '$src' ] && echo asset=ok || echo asset=MISSING; }; chronyc tracking 2>/dev/null | awk '/RMS offset/{print \"rms_ms=\" \$4*1000}' || echo chrony=none" 2>&1 | tr '\n' ' ')" \
    && STATUS[$c]="ok: $out" || STATUS[$c]="UNREACHABLE: $out"
  log "preflight $c @ $( [ "$HOST_MODE" = local ] && echo local || host_of "$c"): ${STATUS[$c]}"
done
log "gNB PC HEAD $(git rev-parse --short HEAD)"

# ---- launch ----
T="$(awk -v n="$(date +%s.%N)" -v d="$START_DELAY" 'BEGIN{printf "%.3f", n+d}')"
log "start time T=$T ($(date -d "@$T" +%H:%M:%S.%3N))"
declare -A LPID
for c in "${CAMS[@]}"; do
  case "${STATUS[$c]}" in UNREACHABLE*) log "skip $c (unreachable)"; continue;; esac
  var="CAM_ARGS_$c"; K="$(camK "$c")"
  cmd="P5G_SYNC=$SYNC_MODE P5G_SYNC_MAX_MS=$SYNC_MAX_MS ./run_sender.sh $RELAY_HOST --to recv$K --stream-id $c ${!var} --duration $DURATION --start-at $T"
  log "launch $c: $cmd"
  on_host "$c" "$cmd" > "$RD/senders/$c.launch.log" 2>&1 &
  LPID[$c]=$!
done
# resolved configuration record
"$PY" - "$RD" "$T" "$HOST_MODE" "$SCEN" <<'PYEOF' "${CAMS[@]}"
import json, sys, subprocess, datetime
rd, T, mode, scen, *cams = sys.argv[1:]
rec = {"scenario": json.load(open(scen)), "start_time_epoch": float(T), "start_time_local": datetime.datetime.fromtimestamp(float(T)).isoformat(),
       "host_mode": mode, "gnb_pc_git_head": subprocess.run(["git","rev-parse","HEAD"],capture_output=True,text=True).stdout.strip(),
       "launch_logs": {c: f"senders/{c}.launch.log" for c in cams}}
json.dump(rec, open(f"{rd}/experiment.json", "w"), indent=2)
PYEOF

# ---- wait for the senders ----
for c in "${!LPID[@]}"; do
  if wait "${LPID[$c]}"; then log "$c finished"; else log "$c FAILED (see senders/$c.launch.log)"; fi
done
sleep 2

# ---- collect sender traces ----
for c in "${CAMS[@]}"; do
  case "${STATUS[$c]}" in UNREACHABLE*) continue;; esac
  mkdir -p "$RD/senders/$c"
  if [ "$HOST_MODE" = "local" ]; then
    src="$(ls -td results/*-sender-$c 2>/dev/null | head -1)"; [ -n "$src" ] && cp -r "$src/app" "$RD/senders/$c/" && log "collected $c from $src"
  else
    remote="$(on_host "$c" "ls -td results/*-sender-$c 2>/dev/null | head -1" 2>/dev/null | tr -d '\r')"
    if [ -n "$remote" ]; then rsync -aq "$HOST_USER@$(host_of "$c"):~/$(repo_of "$c")/$remote/app/" "$RD/senders/$c/app/" && log "collected $c from $(host_of "$c"):$remote" || log "collect $c FAILED"; fi
  fi
done

# ---- stop receivers, verify, report ----
cleanup; trap - EXIT
"$PY" analysis/verify_run.py "$RD" 2>&1 | tail -3 | tee -a "$LOG"
"$PY" analysis/exp_run_report.py "$RD" 5 > "$RD/report.txt" 2>&1 && log "report: $RD/report.txt"
sed -n '/## A\./,/## B\./p' "$RD/report.txt" | grep -E "^cam|A2|selected|^  cam" | head -20
log "done: $RD"

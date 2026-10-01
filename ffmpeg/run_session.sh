#!/usr/bin/env bash
# gNB PC: one command for a whole OTA session — core + gNB, the five phones, the batch, teardown and analysis.
#
#   ./ffmpeg/run_session.sh ffmpeg/experiments/batch-5ue-3rounds.json [label] [--prepare-only] [--no-post]
#
# 0. preflight (nothing is started if anything fails): no gNB/receiver running, control port free; every laptop of
#    the batch's scenarios reachable, same git HEAD as this PC, Wi-Fi off, suspend masked, chrony <= 1 ms, its phone
#    authorised for adb, unlocked, not in battery saver (ffmpeg/scripts/phone.sh check).
# 1. core + gNB: ./run_gnb_core.sh <label> in its own session (setsid; Ctrl-C here does not hit the gNB); waits for
#    "N2: Connection to AMF ... completed" and "==== gNB started ===" (120 s), aborts if the gNB process dies.
# 2. phones: airplane mode off, wait for a UE address of the core's pool (60 s); one airplane on/off retry.
# 3. USB tethering on (phone.sh tether on), then the end-to-end check from every laptop: 10.53.1.1 must be routed via the
#    phone and answer pings (phone.sh path) — i.e. attached, PDU session up, tethering up, nothing over Wi-Fi/sync LAN.
#    One repair attempt (tethering off/on, airplane cycle). Any phone still failing -> no experiment, teardown.
# 4. the batch, run by run (ffmpeg/run_experiment.sh --rotate R). Before EACH run the five paths are checked again and a
#    broken phone is repaired once; a run whose phone stays broken still runs and is marked INVALID by its preflight.
#    --prepare-only stops after step 3 (rehearsal: everything comes up, then everything goes down again).
# 5. teardown (always, also on Ctrl-C or errors): tethering off and airplane mode on for every phone, the gNB stopped with
#    SIGINT (its traces get their footers, run_gnb_core.sh takes the core down), then `run_batch.sh post <session>`
#    (unless --no-post / --prepare-only / no run happened).
# Log: results/<session>/session.log (+ the batch log of every run under runs/).
set -uo pipefail
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(cd "$TREE/.." && pwd)"; cd "$ROOT"
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3
BATCH=""; LABEL=""; PREP=0; POST=1
for a in "$@"; do case "$a" in --prepare-only) PREP=1;; --no-post) POST=0;; -*) echo "unknown option $a" >&2; exit 2;;
  *) if [ -z "$BATCH" ]; then BATCH="$a"; else LABEL="$a"; fi;; esac; done
[ -f "$BATCH" ] || { echo "usage: run_session.sh <batch.json> [label] [--prepare-only] [--no-post]" >&2; exit 2; }
LABEL="${LABEL:-$(basename "$BATCH" .json)}"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
ATTACH_S=60; GNB_UP_S=120; CHRONY_MAX_MS=1
TMPLOG="$(mktemp -t p5g-session.XXXX)"; SESSION=""; GNBC=""; RAN_RUNS=0; PHONES_UP=0
log() { local m="[session $(date +%H:%M:%S)] $*"; echo "$m"; if [ -n "$SESSION" ]; then echo "$m" >> "$SESSION/session.log"; else echo "$m" >> "$TMPLOG"; fi; }
die() { log "ABORT: $*"; exit 1; }

# ---- the batch's runs and its laptops (cam ids in numeric order; host/user/repo from the scenarios) ----
PARSED="$("$PY" - "$BATCH" <<'PYEOF'
import json, sys, shlex
b = json.load(open(sys.argv[1])); runs = b.get("runs") or sys.exit("not a batch file (no 'runs')")
hosts = {}; user = repo = None
for r in runs:
    s = json.load(open(r["scenario"]))
    if s.get("tree", "ffmpeg") != "ffmpeg": sys.exit(f"{r['scenario']}: not an ffmpeg scenario")
    h = s.get("hosts", {}); user = user or h.get("user", "songmu"); repo = repo or h.get("repo", "private-5g-industrial")
    for c, v in s["cams"].items():
        hosts[int(c[3:])] = v.get("host") or h.get("ip_pattern", "192.168.77.1{K}").replace("{K}", c[3:])
print("RUNS=(" + " ".join(shlex.quote(f"{r['scenario']}|{int(r.get('rotate', 0))}") for r in runs) + ")")
print("HOSTS=(" + " ".join(shlex.quote(hosts[k]) for k in sorted(hosts)) + ")")
print(f"HUSER={shlex.quote(user)}; HREPO={shlex.quote(repo)}; PAUSE={int(b.get('pause_s', 10))}")
PYEOF
)" || { echo "[session] cannot use $BATCH as a batch file (above); nothing started" >&2; exit 2; }   # checked BEFORE eval:
eval "$PARSED"                                                       # an empty parse must never reach the gNB start
[ "${#RUNS[@]}" -gt 0 ] && [ "${#HOSTS[@]}" -gt 0 ] || { echo "[session] the batch has no runs or no laptops; nothing started" >&2; exit 2; }
rsh() { local h="$1"; shift; ssh $SSH_OPTS "$HUSER@$h" "cd ~/$HREPO && $*"; }
on_all() {   # on_all <tag> <remote command>: run on every laptop in parallel; prints "<host> <rc> <last line>"; returns #failed
  local tag="$1" cmd="$2" h fails=0; declare -A P
  for h in "${HOSTS[@]}"; do rsh "$h" "$cmd" > "$TMPLOG.$tag.$h" 2>&1 & P[$h]=$!; done
  for h in "${HOSTS[@]}"; do wait "${P[$h]}"; local rc=$?; log "  $h: $(tail -1 "$TMPLOG.$tag.$h" | tr -d '\r')"; [ "$rc" = 0 ] || fails=$((fails + 1)); done
  return "$fails"
}

# ---- teardown (always) ----
teardown() {
  set +e; trap - INT TERM
  if [ "$PHONES_UP" = 1 ]; then
    log "teardown: phones -> tethering off, airplane mode on"
    on_all down "ffmpeg/scripts/phone.sh tether off; ffmpeg/scripts/phone.sh airplane on"
  fi
  if [ -n "$GNBC" ] && kill -0 "$GNBC" 2>/dev/null; then
    log "teardown: stopping the gNB (SIGINT -> traces flushed) and the core"
    sudo -n pkill -INT -x gnb 2>/dev/null
    for i in $(seq 1 60); do kill -0 "$GNBC" 2>/dev/null || break; sleep 1; done
    kill -0 "$GNBC" 2>/dev/null && { log "WARNING: run_gnb_core.sh still running after 60 s -> TERM"; kill -TERM "$GNBC"; sleep 5; }
  fi
  if [ -n "$SESSION" ] && [ "$POST" = 1 ] && [ "$PREP" = 0 ] && [ "$RAN_RUNS" -gt 0 ]; then
    log "post-processing every run of $SESSION (verify, report, fusion, graphs, batch summary)"
    "$TREE/run_batch.sh" post "$SESSION" 2>&1 | tee -a "$SESSION/session.log" | tail -25
  fi
  [ -n "$SESSION" ] && log "session finished: $SESSION"
  rm -f "$TMPLOG" "$TMPLOG".*
}
trap teardown EXIT
trap 'log "Ctrl-C: stopping after cleanup"; exit 130' INT TERM

# ---- 0. preflight ----
log "batch $BATCH: ${#RUNS[@]} runs, laptops ${HOSTS[*]}, label $LABEL$([ $PREP = 1 ] && echo ', PREPARE ONLY')"
pgrep -x gnb >/dev/null && die "a gNB is already running (stop ./run_gnb_core.sh first)"
pgrep -x video_receiver >/dev/null && die "video_receiver processes are still running"
ss -ltn 2>/dev/null | grep -q ":${P5G_CONTROL_PORT:-8765} " && die "control port ${P5G_CONTROL_PORT:-8765} is in use"
sudo -n true 2>/dev/null || die "sudo needs a password on this PC (the gNB runs under sudo)"
HEAD="$(git rev-parse --short HEAD)"
log "preflight: laptops (HEAD $HEAD, Wi-Fi off, suspend masked, chrony <= ${CHRONY_MAX_MS} ms, phone ready)"
CHK="h=\$(git rev-parse --short HEAD); w=\$(nmcli radio wifi); s=\$(systemctl is-enabled suspend.target 2>/dev/null); c=\$(chronyc tracking | awk '/RMS offset/{printf \"%.3f\", \$4*1000}');
p=\$(ffmpeg/scripts/phone.sh check | tail -1); echo \"head=\$h wifi=\$w suspend=\$s chrony_ms=\$c phone=\$p\";
[ \"\$h\" = $HEAD ] && [ \"\$w\" = disabled ] && [ \"\$s\" = masked ] && [ \"\$p\" = ready ] && awk -v c=\"\$c\" 'BEGIN{exit !(c <= $CHRONY_MAX_MS)}'"
on_all pre "$CHK" || die "preflight failed on the laptop(s) above (HEAD: git pull from this PC; Wi-Fi: nmcli radio wifi off; phone: see message)"

# ---- 1. core + gNB ----
log "starting core + gNB (./run_gnb_core.sh $LABEL)"
OLD_CUR="$(readlink results/CURRENT 2>/dev/null || true)"
setsid nohup ./run_gnb_core.sh "$LABEL" > "$TMPLOG.gnbcore" 2>&1 < /dev/null & GNBC=$!
end=$(( $(date +%s) + GNB_UP_S ))
while :; do
  kill -0 "$GNBC" 2>/dev/null || { tail -15 "$TMPLOG.gnbcore"; die "run_gnb_core.sh exited during start-up (log above)"; }
  CUR="$(readlink results/CURRENT 2>/dev/null || true)"
  if [ -n "$CUR" ] && [ "$CUR" != "$OLD_CUR" ] && [ -f "results/$CUR/gnb/gnb_stdout.log" ]; then
    SESSION="results/$CUR"
    grep -q "==== gNB started ===" "$SESSION/gnb/gnb_stdout.log" && grep -q "N2: Connection to AMF.*completed" "$SESSION/gnb/gnb_stdout.log" && break
  fi
  [ "$(date +%s)" -lt "$end" ] || { tail -15 "$TMPLOG.gnbcore"; die "gNB not up within ${GNB_UP_S}s (no 'gNB started' / AMF connection)"; }
  sleep 2
done
cat "$TMPLOG" >> "$SESSION/session.log"; cp "$BATCH" "$SESSION/batch.json"
log "gNB up: $(grep -m1 '^Cell pci' "$SESSION/gnb/gnb_stdout.log")  session $SESSION"

# ---- 2 + 3. phones: attach, tethering, end-to-end path ----
PHONES_UP=1
bring_up() {   # bring_up <host>: attach + tether + path, with one repair attempt; prints the result line
  local h="$1"
  rsh "$h" "ffmpeg/scripts/phone.sh airplane off >/dev/null; ffmpeg/scripts/phone.sh wait-attach $ATTACH_S >/dev/null || { ffmpeg/scripts/phone.sh airplane on >/dev/null; sleep 3; ffmpeg/scripts/phone.sh airplane off >/dev/null; ffmpeg/scripts/phone.sh wait-attach $ATTACH_S; } || exit 11
    ffmpeg/scripts/phone.sh tether on >/dev/null; if ! ffmpeg/scripts/phone.sh path >/dev/null; then ffmpeg/scripts/phone.sh tether off >/dev/null; sleep 2; ffmpeg/scripts/phone.sh tether on >/dev/null; fi
    a=\$(ffmpeg/scripts/phone.sh wait-attach 5 | tail -1); p=\$(ffmpeg/scripts/phone.sh path | tail -1); echo \"\$a \$p\"; case \"\$p\" in path=ok*) exit 0;; *) exit 12;; esac"
}
check_paths() {   # every laptop: path ok? repair the broken ones once; returns the number still broken
  local h bad=0; declare -A P
  for h in "${HOSTS[@]}"; do rsh "$h" "ffmpeg/scripts/phone.sh path" > "$TMPLOG.path.$h" 2>&1 & P[$h]=$!; done
  for h in "${HOSTS[@]}"; do
    if wait "${P[$h]}"; then continue; fi
    log "  $h: $(tail -1 "$TMPLOG.path.$h") -> repairing"
    if bring_up "$h" > "$TMPLOG.fix.$h" 2>&1; then log "  $h: repaired: $(tail -1 "$TMPLOG.fix.$h")"; else log "  $h: STILL BROKEN: $(tail -1 "$TMPLOG.fix.$h")"; bad=$((bad + 1)); fi
  done
  return "$bad"
}
log "phones: airplane off -> attach (${ATTACH_S}s, one retry) -> USB tethering -> ping 10.53.1.1 through the phone"
declare -A BP; for h in "${HOSTS[@]}"; do bring_up "$h" > "$TMPLOG.up.$h" 2>&1 & BP[$h]=$!; done
FAILED=0; for h in "${HOSTS[@]}"; do if wait "${BP[$h]}"; then log "  $h: $(tail -1 "$TMPLOG.up.$h")"; else log "  $h: FAILED: $(tail -1 "$TMPLOG.up.$h")"; FAILED=$((FAILED + 1)); fi; done
[ "$FAILED" = 0 ] || die "$FAILED phone(s) not on the 5G path; no experiment started"
NUE="$(tail -60 "$SESSION/gnb/gnb_stdout.log" | grep -oE '^ *[0-9]+ +[0-9a-f]{4} *\|' | awk '{print $2}' | sort -u | wc -l)"
log "all ${#HOSTS[@]} phones attached and tethered, path to the core verified (gNB metrics table shows $NUE RNTIs)"
[ "$PREP" = 1 ] && { log "--prepare-only: everything is up; tearing down"; exit 0; }

# ---- 4. the batch, run by run, with a path check + repair before each ----
i=0
for entry in "${RUNS[@]}"; do
  i=$((i + 1)); scen="${entry%|*}"; rot="${entry##*|}"
  log "run $i/${#RUNS[@]}: $scen --rotate $rot - checking the five paths first"
  check_paths || log "WARNING: a phone is still off the 5G path; this run will be marked INVALID by its preflight"
  RAN_RUNS=$((RAN_RUNS + 1))
  if "$TREE/run_experiment.sh" "$scen" --rotate "$rot" 2>&1 | tee -a "$SESSION/session.log" | grep -E "preflight|RESULT|ABORT|WARNING|done:" ; then :; fi
  [ "$i" -lt "${#RUNS[@]}" ] && sleep "$PAUSE"
done
log "batch complete: ${#RUNS[@]} runs"

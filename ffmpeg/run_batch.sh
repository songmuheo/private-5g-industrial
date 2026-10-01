#!/usr/bin/env bash
# gNB PC: run a batch of experiments back to back inside ONE live gNB session (./run_gnb_core.sh in terminal 1), then
# summarise them; or, after the session was stopped, re-analyse every run with the gNB traces complete.
#
#   ./ffmpeg/run_batch.sh ffmpeg/experiments/batch-5ue.json      run the batch (each run: ffmpeg/run_experiment.sh --rotate R)
#   ./ffmpeg/run_batch.sh post [results/<session>]               after Ctrl-C in terminal 1: verify + report + fusion + graphs
#                                                                for every run, then the batch summary (default: newest session)
#
# Batch file: {"name": ..., "pause_s": 10, "runs": [{"scenario": "ffmpeg/experiments/5ue-A-uniform1500.json", "rotate": 0}, ...]}.
# Every run lands in results/<session>/runs/<ts>-<name>-r<R>/ (gnb/ and core/ linked to the session). A failed or INVALID run
# is recorded and the batch goes on. Ctrl-C stops the batch after cleaning up the current run (run_experiment.sh's trap).
set -euo pipefail
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(cd "$TREE/.." && pwd)"; cd "$ROOT"
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3

if [ "${1:-}" = "post" ]; then
  SESSION="${2:-}"; [ -n "$SESSION" ] || SESSION="$(ls -td results/*/runs 2>/dev/null | head -1 | xargs -r dirname)"
  [ -d "$SESSION/runs" ] || { echo "no session with runs/ (give results/<session>)" >&2; exit 1; }
  pgrep -x gnb >/dev/null && { echo "the gNB is still running: stop ./run_gnb_core.sh first (its traces get their footers on exit)" >&2; exit 1; }
  FAILS=0
  for RD in "$SESSION"/runs/*/; do   # every step guarded: one failing run never stops the others or the summary
    RD="${RD%/}"; echo "== $RD"; mkdir -p "$RD/analysis"
    rm -f "$RD/analysis/VERIFY_FAILED"
    if "$PY" analysis/verify_run.py "$RD" > "$RD/analysis/verify.log" 2>&1; then tail -1 "$RD/analysis/verify.log"
    else echo "   verify FAILED (see $RD/analysis/verify.log)"; FAILS=$((FAILS + 1)); tail -3 "$RD/analysis/verify.log" > "$RD/analysis/VERIFY_FAILED"; fi   # summary excludes it
    "$PY" analysis/exp_run_report.py "$RD" 5 > "$RD/analysis/report.txt" 2>&1 || { echo "   report failed"; FAILS=$((FAILS + 1)); }
    if "$PY" -c "import json,sys; sys.exit(0 if json.load(open('$RD/scenario.json')).get('groups') else 1)" 2>/dev/null; then
      "$PY" analysis/fusion_report.py "$RD" > "$RD/analysis/fusion_run.log" 2>&1 || { echo "   fusion failed (see $RD/analysis/fusion_run.log)"; FAILS=$((FAILS + 1)); }
    fi
    "$PY" analysis/plot_run.py "$RD" > /dev/null 2>&1 || { echo "   graphs failed"; FAILS=$((FAILS + 1)); }
  done
  "$PY" analysis/batch_summary.py "$SESSION" || FAILS=$((FAILS + 1))
  [ "$FAILS" = 0 ] || { echo "[batch] post: $FAILS step(s) failed (see the logs above)"; exit 1; }
  exit 0
fi

BATCH="${1:?usage: run_batch.sh <batch.json> | post [results/<session>]}"
[ -f "$BATCH" ] || { echo "no such batch file: $BATCH" >&2; exit 1; }
pgrep -x gnb >/dev/null && [ -L results/CURRENT ] || { echo "start the gNB session first: ./run_gnb_core.sh <label> (terminal 1)" >&2; exit 1; }
SESSION="results/$(readlink results/CURRENT)"
mapfile -t RUNS < <("$PY" -c "
import json, sys
b = json.load(open('$BATCH'))
for r in b['runs']: print(r['scenario'], int(r.get('rotate', 0)))
")
PAUSE="$("$PY" -c "import json; print(int(json.load(open('$BATCH')).get('pause_s', 10)))")"
BLOG="$SESSION/batch-$(date +%Y%m%d-%H%M%S).log"; blog() { echo "[batch $(date +%H:%M:%S)] $*" | tee -a "$BLOG"; }
blog "batch $BATCH: ${#RUNS[@]} runs into $SESSION/runs/ (pause ${PAUSE}s between runs)"
STOP=0; trap 'STOP=1' INT
i=0
for line in "${RUNS[@]}"; do
  i=$((i + 1)); scen="${line% *}"; rot="${line##* }"
  if [ "$STOP" = 1 ]; then blog "stopped by Ctrl-C before run $i"; break; fi
  blog "run $i/${#RUNS[@]}: $scen --rotate $rot"
  if "$TREE/run_experiment.sh" "$scen" --rotate "$rot"; then blog "run $i done"; else blog "run $i FAILED (exit $?) - continuing"; fi
  if [ "$STOP" = 1 ]; then blog "stopped by Ctrl-C after run $i"; break; fi
  if [ "$i" -lt "${#RUNS[@]}" ]; then sleep "$PAUSE" || true; fi   # Ctrl-C during the pause: fall through to the STOP check
done
"$PY" analysis/batch_summary.py "$SESSION" | tee -a "$BLOG" || true
blog "batch finished. Stop the gNB (Ctrl-C in terminal 1), then: ./ffmpeg/run_batch.sh post $SESSION"

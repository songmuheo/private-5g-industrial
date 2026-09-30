#!/usr/bin/env bash
# Reference code of related systems (read-only, never built or run from here; used to read how they stream and
# schedule). Shallow clones into docs/reference_code/<name>/, pinned to the commits in reference_code.lock.
#   docs/reference_code/fetch.sh            # clone / update to the pinned commits
#   docs/reference_code/fetch.sh --pin      # rewrite the lock with the clones' current HEADs
# The clones are git-ignored; only this script, the lock and README.md are versioned (CLAUDE.md rule 5).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; LOCK="$HERE/reference_code.lock"
if [ "${1:-}" = "--pin" ]; then
  { echo "# name  url  commit  (docs/reference_code/fetch.sh --pin)"; for d in "$HERE"/*/; do n="$(basename "$d")"; [ -d "$d/.git" ] || continue
    echo "$n $(git -C "$d" remote get-url origin) $(git -C "$d" rev-parse HEAD)"; done; } > "$LOCK"; cat "$LOCK"; exit 0
fi
grep -v '^#' "$LOCK" | while read -r name url commit; do
  [ -n "$name" ] || continue
  d="$HERE/$name"
  if [ ! -d "$d/.git" ]; then git clone --quiet --filter=blob:none "$url" "$d"; fi
  git -C "$d" fetch --quiet origin "$commit" 2>/dev/null || git -C "$d" fetch --quiet origin
  git -C "$d" checkout --quiet "$commit"
  printf '  %-28s %s  %s\n' "$name" "$(git -C "$d" rev-parse --short HEAD)" "$(git -C "$d" log -1 --format=%cs)"
done

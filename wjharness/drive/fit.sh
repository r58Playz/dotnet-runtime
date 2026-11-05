#!/bin/bash
# fit.sh [max-minutes, default 120] -- block until the box is fit to measure: the dev server answers, no build daemon
# is alive, and lib/preflight.mjs passes (CPU contention, /tmp space, stray harness browsers...). Re-checks every 60 s.
# Exit 0 when fit, 1 if it never became fit. Every round launcher used to carry its own copy of this loop.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
max=${1:-120}
for i in $(seq 1 "$max"); do
  bash "$WJ_HARNESS/drive/ensure-server.sh" >/dev/null 2>&1
  bash "$WJ_HARNESS/build/killdaemons.sh" >/dev/null 2>&1 || true
  if node -e 'import(process.argv[1]).then(m => { m.preflight({ log: () => {} }); process.exit(0); })' "$WJ_HARNESS/lib/preflight.mjs" >/dev/null 2>&1; then
    [ "$i" -gt 1 ] && echo "fit after $i checks"
    exit 0
  fi
  sleep 60
done
echo "preflight never passed in $max minutes" >&2
exit 1

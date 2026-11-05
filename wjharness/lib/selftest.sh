#!/bin/bash
# wj selftest -- every self-check that needs no browser, capture or deploy. Seconds.
#   syntax      node --check on every .mjs/.js, py_compile on every .py, bash -n on every .sh
#   verdict     lib/verdict.mjs's selftest (real fault signatures from archived logs)
#   hotinsn     read/hotinsn.py --selftest (the instruction classifier against known encodings)
#   enctest     the encoder gates (enctest/run.sh: framers, serializer, stub banks executed in node)
#   paths       no tool names this machine's paths (the portability gate), and paths.mjs resolves
#   corpus      read/faultcensus.mjs --disjoint over $WJ_STATE/corpus, when it exists (--no-corpus skips; slow)
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
H=$WJ_HARNESS
fail=0
step () { printf '%-9s ' "$1"; }
ok () { echo "ok${1:+  $1}"; }
no () { echo "FAIL${1:+  $1}"; fail=1; }

step syntax
errs=$(
  find "$H" -name node_modules -prune -o \( -name '*.mjs' -o -name '*.js' \) -print0 | while IFS= read -r -d '' f; do
    case "$f" in */memaudit/*.js|*/j2d/cheerpj/*) continue ;; esac   # page-side snippets, evaluated in the browser
    node --check "$f" >/dev/null 2>&1 || echo "  node --check $f"; done
  find "$H" -name '*.py' -print0 | while IFS= read -r -d '' f; do
    python3 -c 'import ast,sys; ast.parse(open(sys.argv[1]).read(), sys.argv[1])' "$f" 2>/dev/null || echo "  python $f"; done
  find "$H" -name '*.sh' -print0 | while IFS= read -r -d '' f; do bash -n "$f" 2>/dev/null || echo "  bash -n $f"; done
  bash -n "$H/wj" || echo "  bash -n wj")
[ -z "$errs" ] && ok "$(find "$H" \( -name '*.mjs' -o -name '*.py' -o -name '*.sh' -o -name '*.js' \) | wc -l) files" || { no; echo "$errs"; }

step verdict
out=$(node -e 'import(process.argv[1]).then(m => process.exit(m.selftest() ? 0 : 1))' "$H/lib/verdict.mjs" 2>&1)
[ $? -eq 0 ] && ok || { no; echo "$out" | tail -20; }

step hotinsn
out=$(python3 "$H/read/hotinsn.py" --selftest 2>&1)
[ $? -eq 0 ] && ok "$(echo "$out" | tail -1)" || { no; echo "$out" | tail -20; }

step enctest
if command -v gcc >/dev/null; then
  out=$(bash "$H/enctest/run.sh" 2>&1)
  [ $? -eq 0 ] && ok "$(echo "$out" | grep -c '^PASS\|0 failures') gates" || { no; echo "$out" | tail -20; }
else echo "skipped (no gcc)"; fi

step paths
hits=$(grep -rnE "$(printf '%s|Documents/' "${USER:-nobody}")" "$H" --include='*.mjs' --include='*.py' --include='*.sh' --include='*.js' --include=wj --exclude=selftest.sh 2>/dev/null)
node "$H/lib/paths.mjs" --json >/dev/null 2>&1 || hits="$hits
  lib/paths.mjs does not resolve"
[ -z "$hits" ] && ok || { no "a tool names this machine"; echo "$hits"; }

step corpus
if [ "${1:-}" = "--no-corpus" ]; then echo "skipped (--no-corpus)"
elif [ -d "$WJ_STATE/corpus" ]; then
  out=$(node "$H/read/faultcensus.mjs" --disjoint "$WJ_STATE/corpus" 2>&1)
  [ $? -eq 0 ] && ok "$(echo "$out" | tail -1)" || { no; echo "$out" | tail -20; }
else echo "skipped (no $WJ_STATE/corpus)"; fi

exit $fail

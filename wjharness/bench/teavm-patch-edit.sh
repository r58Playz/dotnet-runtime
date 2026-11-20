#!/usr/bin/env bash
# Edit the newest TeaVM patch (bench/teavm-patches/<last>.patch) in teavm-cli.sh's worktree:
#   wj bench/teavm-patch-edit begin    reset the worktree to the pin, commit every earlier patch as a local base, apply
#                                      the last one; prints the worktree
#   ... edit files in the worktree ...
#   wj bench/teavm-patch-edit save     rewrite the last patch as the worktree's diff against that base (its leading
#                                      description lines, up to the first "diff --git", are kept)
# Then wj bench/teavm-cli --force. The base commit is worktree-local and the next teavm-cli.sh run resets it away.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../lib/env.sh"
WT="$WJ_RUNS/.tmp/teavm-cli-src"
PATCHES=("$HERE"/teavm-patches/*.patch)
LAST="${PATCHES[-1]}"
[ -e "$WT/.git" ] || { echo "no worktree at $WT: run wj bench/teavm-cli once" >&2; exit 1; }
case "${1:-}" in
  begin)
    git -C "$WT" reset -q --hard 9be5eca57
    git -C "$WT" clean -fdq
    for p in "${PATCHES[@]:0:${#PATCHES[@]}-1}"; do git -C "$WT" apply "$p"; done
    git -C "$WT" add -A
    git -C "$WT" -c user.name=wj -c user.email=wj@localhost -c commit.gpgsign=false commit -q --allow-empty -m "wj base"
    git -C "$WT" apply --allow-empty "$LAST"
    echo "$WT  (editing $(basename "$LAST"))" ;;
  save)
    BASE=$(git -C "$WT" log --format=%H --grep='^wj base$' -1)
    [ -n "$BASE" ] || { echo "no wj base commit: run begin first" >&2; exit 1; }
    git -C "$WT" add -N .
    TMP=$(mktemp)
    awk '/^diff --git/ {exit} {print}' "$LAST" > "$TMP"
    git -C "$WT" diff "$BASE" -- . >> "$TMP"
    mv "$TMP" "$LAST"
    echo "$(basename "$LAST"): $(grep -c '^diff --git' "$LAST") files" ;;
  *) echo "usage: teavm-patch-edit.sh begin|save" >&2; exit 1 ;;
esac

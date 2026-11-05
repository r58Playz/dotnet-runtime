#!/bin/bash
# Kill leftover build daemons before a measurement. Exit 0 = safe to measure.
#
# TWO hazards, both paid for:
#
# 1) preflight matches '[V]BCSCompiler|[M]SBuild.dll'. A filter that checks only the Roslyn half misses
#    the MSBuild half -- which voided R200's shadow_bytes=8000 arm: preflight warned, --force ran anyway,
#    and the daemon exited PARTWAY THROUGH, taxing that arm and not its control (CLAUDE.md predicts
#    exactly this).
#
# 2) A `-f` (full command line) match MATCHES THE SEARCHER. This has now bitten three times in one
#    session: twice as `pkill -f <pat>` killing the invoking shell (exit 144), and once here, where the
#    pattern sat in a heredoc and so became part of the outer shell's own argv. The bracket trick
#    ('[V]BCS') only defends against the literal pattern string, NOT against a shell whose argv happens
#    to contain the plain word. So: match the full command line to FIND candidates, then keep only
#    those whose comm is the daemon's own executable, and never kill self or an ancestor.
SELF=$$
for i in 1 2 3 4 5; do
  P=""
  for p in $(pgrep -f 'BCSCompiler|MSBuild\.dll' 2>/dev/null); do
    [ "$p" = "$SELF" ] && continue
    c=$(ps -p "$p" -o comm= 2>/dev/null)
    case "$c" in dotnet|VBCSCompiler|MSBuild) P="$P $p";; esac
  done
  P=$(echo $P)
  [ -z "$P" ] && { echo "daemons: clean"; exit 0; }
  echo "daemons: killing$P"
  for p in $P; do kill -9 "$p" 2>/dev/null; done
  sleep 2
done
echo "daemons: STILL ALIVE -- do not measure"; exit 1

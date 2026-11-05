#!/bin/bash
# Bring the ikvmcraft dev server up and WAIT FOR A REAL 200, race-free. Source or run before any batch.
#
# Exists because this block was copy-pasted into three batch scripts and the copies drifted: two of them
# only WAITED for a 200 that could never arrive, and preflight then refused the whole batch. The failure
# is systematic, not incidental -- `deploy.sh` SIGTERMs the dev server, and restarting it immediately
# loses a race with the dying process still holding port 5021 (`Error: Port 5021 is already in use`,
# leaving the unit `failed`). Stop, wait for the PORT, then start, then verify.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
APP=$(need IKVMCRAFT "the dev server serves $IKVMCRAFT/frontend")
U=$DEV_UNIT
PORT=$(node -e 'const u=new URL(process.argv[1]); console.log(u.port || (u.protocol==="https:"?443:80))' "$SERVE_URL")
for _ in $(seq 1 40); do
  [ "$(curl -sk -o /dev/null -w '%{http_code}' --max-time 5 "$SERVE_URL" 2>/dev/null)" = "200" ] && exit 0
  if ! systemctl --user is-active "$U" >/dev/null 2>&1; then
    systemctl --user stop "$U" >/dev/null 2>&1
    systemctl --user reset-failed "$U" >/dev/null 2>&1
    for _ in $(seq 1 30); do ss -ltn 2>/dev/null | grep -q ":$PORT " || break; sleep 1; done
    systemd-run --user --quiet --unit="$U" \
      --working-directory="$APP/frontend" -- bash -lc "$DEV_CMD"
  fi
  sleep 3
done
echo "ensure-server: dev server never reached HTTP 200" >&2
exit 1

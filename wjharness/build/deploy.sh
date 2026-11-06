#!/bin/bash
# deploy.sh <marker> [marker...] — put a freshly built runtime in front of the browser, and PROVE it.
#
# THIS EXISTS BECAUSE A WHOLE MEASUREMENT MATRIX RAN ON A STALE RUNTIME.
#
# Copying dotnet-jspi.zip over statics/dotnet.zip and unzipping it into statics/dotnet is NOT a deploy.
# statics/dotnet is only the runtime PACK that `dotnet publish` reads (loader/IkvmWasm.csproj:50); the
# bytes the page actually loads are frontend/public/_framework/dotnet.native.<hash>.wasm, which only
# changes when the loader is republished and copied over. Skip that and the browser keeps running the
# previous runtime, silently.
#
# What made it silent, and why the obvious check does not work: `[harness] env override: NAME=VALUE` is
# printed by index.ts's override loop for ANY name, before the runtime exists. Seeing
# "env override: MONO_WASM_JIT_RAISE_FRAME=0" in the log proves the query parameter was parsed by the
# PAGE. It says nothing about whether the runtime has ever heard of that variable — an unknown env var
# is simply ignored. Four arms of a matrix "responded" to a knob that was not in the binary.
#
# So the check has to be against the served bytes: every marker must appear in the wasm the page will
# fetch. Pass the string constants your new code introduced (an env var name, a printf tag).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
. "$ROOT/../lib/env.sh"
APP=$(need IKVMCRAFT "deploy publishes the ikvmcraft loader")
ZIP=$(need DOTNET_ZIP)

[ $# -ge 1 ] || { echo "usage: deploy.sh <marker> [marker...]   (string constants your change added)" >&2; exit 2; }

SECONDS_START=$(date +%s)   # window for the systemd-oomd attribution on failure

cd "$APP" || exit 1
[ -f "$ZIP" ] || { echo "deploy: no $ZIP" >&2; exit 1; }

echo "deploy: staging runtime pack"
cp "$ZIP" statics/dotnet.zip
rm -rf statics/dotnet
unzip -q -o statics/dotnet.zip -d statics/dotnet

# `dotnet publish` does not clean wwwroot/_framework, and the runtime wasm is content-FINGERPRINTED
# (dotnet.native.<hash>.wasm). A new build therefore ADDS a file next to the old one instead of replacing
# it, and anything that picks "the" wasm by globbing can grab the stale one -- which is how the first
# version of this very script reported MISSING markers against a directory that also held the fresh
# binary. The Makefile's `build` target removes both directories first; do the same.
rm -rf loader/bin/Release/net10.0/publish/wwwroot/_framework frontend/public/_framework

# AND remove loader/{bin,obj} ENTIRELY -- the Makefile's `dotnetclean` does this and it is load-bearing.
#
# dotnet caches AOT output. Deleting only _framework refreshes the RUNTIME (dotnet.native.wasm relinks, so
# the marker check below still passes) while the AOT-compiled MANAGED assemblies are reused from obj/. Any
# change that lives in mono-aot-cross rather than in the runtime -- i.e. anything in method-to-ir.c,
# mini-llvm.c or the shared mini/ code that shapes GENERATED code -- is then silently absent from the thing
# under test, and the marker check cannot see it because the marker is a runtime string.
#
# That happened: a fix to emit_push_lmf (method-to-ir.c) was "tested" three times against cached AOT output
# and scored as having no effect. Do not trade this rebuild time for that class of false negative.
#
# Generated.targets and ikvmc-manifest.g.json are deliberately NOT removed (dotnetclean does): they are
# ikvmc bundle products, independent of the runtime, and regenerating them costs far more than it saves.
rm -rf loader/bin loader/obj

# AOT DEFAULTS TO TRUE HERE, unlike the Makefile (AOT?=false). The app that all previous measurements
# were taken against was AOT-compiled: its dotnet.native.wasm was 188 MB, versus 35 MB for the
# interpreter-only link. Publishing without AOT silently swaps the workload for a ~5x slower one, which
# moved every GC-timing-sensitive result -- the positive control that had failed 4/4 at ~30s stopped
# failing at all and simply timed out mid class-loading. Same class of mistake as the stale runtime: the
# thing under test looked fine, the thing around it had changed.
# PUBLISH IN ITS OWN CGROUP, WITH TEMP FILES ON DISK.
#
# emscripten's link step runs `wasm-opt --post-emscripten -O3 ... -g` over the whole ~190 MB
# dotnet.native.wasm in ONE process that peaks near 9.5 GB. On a 15 GiB box that is ~55% memory pressure
# on user@1000.service, and systemd-oomd (40% for 15s -- the stock upstream default, not a local tweak)
# then kills the highest-pressure scope underneath it. Invoked straight from a terminal, that scope is
# the terminal: shell, editor, agent and every other child share one cgroup. It reaped two ~191-process
# sessions before this was isolated, and from the outside that looks like the machine randomly dying
# mid-deploy. A dedicated scope makes the publish the only thing oomd can reach.
#
# TMPDIR is the same problem from the other side: /tmp is tmpfs here (7.7 GiB, i.e. RAM), and emscripten
# writes linker response files and intermediate wasm there, so the default charges part of the peak to
# RAM twice. BUILD_TMPDIR overrides it; point it at real disk.
# PUBLISH_EXTRA: extra `dotnet publish` arguments, e.g. PUBLISH_EXTRA=-p:IkvmWasmManagedAot=false for the build in
# which no assembly's methods are AOT'd (loader/IkvmWasm.csproj; the wasm is then far below the AOT size check below).
echo "deploy: publishing loader (AOT=${AOT:-true}${PUBLISH_EXTRA:+ $PUBLISH_EXTRA})"
RUNNER=()
if command -v systemd-run >/dev/null 2>&1 && systemd-run --user --scope --quiet -- /bin/true >/dev/null 2>&1; then
	# The scope is NAMED so the oomd journal line reads "ikvmcraft-publish.scope: killed 52 process(es)"
	# instead of an opaque run-rNNN -- that attribution is the whole point of isolating it. The cost of a
	# fixed name is that the unit lingers in a failed state after exactly the kill we expect, and the next
	# run then dies with "Unit ... was already loaded or has a fragment file" -- a confusing error that has
	# nothing to do with the build. Clear it first.
	systemctl --user reset-failed ikvmcraft-publish.scope >/dev/null 2>&1
	systemctl --user stop ikvmcraft-publish.scope >/dev/null 2>&1
	# ManagedOOMPreference=omit + a hard MemoryMax, together.
	#
	# Isolating the scope stopped the collateral damage but did not let the publish finish: wasm-opt holds
	# ~9.5 GB for well over oomd's DefaultMemoryPressureDurationSec=15s, so oomd kept killing it at the same
	# step even with 10 GB free. That is oomd being impatient with an allocation that IS satisfiable, not the
	# box being out of memory -- so exempt this scope from PREEMPTIVE pressure killing.
	#
	# Exempting alone would be reckless: oomd would just pick a different victim (the session, again). So pair
	# it with MemoryMax, which makes the KERNEL oom-kill inside this cgroup and nowhere else if the publish
	# genuinely exceeds its budget. Failure stays attributable and the blast radius stays the publish.
	# PUBLISH_MEM_MAX overrides for a box with a different amount of RAM.
	RUNNER=(systemd-run --user --scope --quiet --nice=10 --unit=ikvmcraft-publish
	        -p ManagedOOMPreference=omit -p MemoryMax="${PUBLISH_MEM_MAX:-11G}"
	        -p MemorySwapMax=infinity)
else
	echo "  WARNING: systemd-run unavailable — the publish shares this shell's cgroup," >&2
	echo "           so an OOM during wasm-opt will kill the caller too." >&2
fi
: "${BUILD_TMPDIR:=/var/tmp/ikvmcraft-build}"
mkdir -p "$BUILD_TMPDIR"

"${RUNNER[@]}" env TMPDIR="$BUILD_TMPDIR" \
	dotnet publish loader/IkvmWasm.csproj -c Release \
	-p:IkvmWasmEnableAot="${AOT:-true}" -p:IkvmWasmEnableWasmOpt="${OPT:-false}" ${PUBLISH_EXTRA:-} \
	--nodereuse:false -v m > /tmp/ikvmcraft-publish.log 2>&1 || {
		echo "deploy: publish FAILED — see /tmp/ikvmcraft-publish.log" >&2
		tail -20 /tmp/ikvmcraft-publish.log >&2
		# Distinguish "the build is broken" from "the host ran out of memory", which need opposite
		# responses: fix the code vs re-run with less running. Without this the log just ends.
		source "$ROOT/faults.sh"
		hits="$(oomd_kills_since "$((SECONDS_START))")"
		[ -n "$hits" ] && { echo "deploy: systemd-oomd fired during the publish — host OOM, not a build error:" >&2
		                    echo "$hits" | tail -3 >&2; }
		echo "deploy: _framework was cleaned before publishing, so the app has NO runtime right now." >&2
		echo "        Re-run deploy.sh; do not measure until it reports ok." >&2
		exit 1
	}

cp -r loader/bin/Release/net10.0/publish/wwwroot/_framework frontend/public/

# THE MAKEFILE'S TWO POST-PUBLISH PATCHES (Makefile:63,65), WHICH THIS SCRIPT USED TO OMIT.
#
# Both fail silently when absent, and the first invalidates any wasm_jit measurement outright:
#
# 1. appendULeb(32768) -> 65535 in the runtime's generated-module builder. That literal is the MAXIMUM
#    field of each generated module's `m.h` shared-memory import (appendU8(2)=memory,
#    appendU8(3)=has-max|shared, appendULeb(256)=min, then the max). The app links with
#    -sMAXIMUM_MEMORY=4294901760 = 65535 pages, and WebAssembly requires an imported shared memory's
#    declared maximum to MATCH the memory actually supplied -- so at 32768 EVERY jiterpreter and wasm_jit
#    module fails to instantiate with a LinkError. The JIT then quietly compiles nothing, which presents
#    as "the JIT got slower", and a per-worker instantiation census would have read zero and been
#    believed as evidence about laziness.
#
# 2. transferredCanvasNames=[".canvas"] in dotnet.native.*.js. Without it the canvas is never
#    transferred to the rendering worker and Minecraft dies at glfwCreateWindow with "GLFW error 65544:
#    Cannot create GL context for [#canvas]" -- which faults.sh deliberately scores as VOID (an
#    ENVIRONMENT failure), so a missing build step masquerades as a flaky GPU and the run is retried
#    forever instead of fixed.
#
# Neither surfaced while the jbox2d bench was the workload: it creates no GL window, and a JIT that
# silently failed to link still yields a plausible (merely slower) ms/step.
echo "deploy: applying post-publish patches"
sed -i 's/this.appendULeb(32768)/this.appendULeb(65535)/' frontend/public/_framework/dotnet.runtime.*.js
# The transferredCanvasNames patch (2.) is GONE: ikvmcraft's Makefile dropped it on 2026-10-21 (commit 246560a, "css"),
# with GL on the main thread by default and index.ts making the canvas resizable itself (glfwOnWindowCreated). This
# script follows the Makefile, so a deploy serves what `make build` serves.

# Verified, not assumed: a sed whose pattern stopped matching after a runtime update exits 0 having
# changed nothing, which is the same silent-wrong-artifact failure this whole script exists to catch.
if ! grep -q 'appendULeb(65535)' frontend/public/_framework/dotnet.runtime.*.js ||
   grep -q 'appendULeb(32768)' frontend/public/_framework/dotnet.runtime.*.js; then
	echo "deploy: FAIL — the appendULeb(32768)->65535 patch did not apply." >&2
	echo "        Generated wasm modules would declare max=32768 against a 65535-page shared memory" >&2
	echo "        and every one would LinkError. Check whether the runtime emitter changed." >&2
	exit 1
fi
echo "  patch OK       appendULeb 65535 (shared-memory import max)"

mapfile -t WS < <(find frontend/public/_framework -name 'dotnet.native.*.wasm')
[ "${#WS[@]}" -ge 1 ] || { echo "deploy: no dotnet.native.*.wasm under frontend/public/_framework" >&2; exit 1; }
if [ "${#WS[@]}" -gt 1 ]; then
	echo "deploy: FAIL — ${#WS[@]} runtime wasms in the served directory; the page picks one from the boot" >&2
	printf '  %s\n' "${WS[@]##*/}" >&2
	echo "  manifest and a marker check on the wrong one proves nothing. Clean and republish." >&2
	exit 1
fi
W="${WS[0]}"
echo "deploy: served wasm = $(basename "$W")  $(( $(stat -c%s "$W") / 1048576 )) MB  ($(stat -c%y "$W" | cut -d. -f1))"
# Size is the cheapest AOT check there is: ~86 MB (82 MiB) with the AOT whitelist (CoreLib, IKVM.Runtime,
# IKVM.ByteCode, System.Memory; 2026-11-05), ~35 MB without. The threshold was 100 MB from when the whole app was
# AOT'd (~188 MB) and warned on every correct deploy since the whitelist. If this number moves by 2x between deploys,
# the workload changed even though the runtime source did not.
if [ "$(stat -c%s "$W")" -lt 60000000 ] && [ "${AOT:-true}" != "false" ]; then
	echo "  WARNING: AOT was requested but the wasm is small — check /tmp/ikvmcraft-publish.log" >&2
fi

# Search the runtime JS as well as the wasm. NOTE markers must be STRING LITERALS: identifiers and comments
# are minified away in dotnet.runtime.<hash>.js, so passing a function name matches nothing even when the
# change is deployed (cost two round trips). Not every runtime change lands in dotnet.native.wasm: the
# jiterpreter's table management, its option plumbing and the whole interp-entry path live in
# src/mono/browser/runtime/*.ts and end up in dotnet.runtime.<hash>.js. Checking only the wasm reported
# "marker MISSING" for a TypeScript change that was in fact deployed correctly, and the FAIL then skipped
# the vite restart below, which broke the very next boot. Identifiers are minified in that bundle, so match
# on string literals only.
RJS="$(ls frontend/public/_framework/dotnet.runtime.*.js 2>/dev/null | head -1)"
# AND dotnet.native.<hash>.js, which is a THIRD place runtime code lands and was missing here: every EM_ASM
# / EM_JS body in mini-wasm.c -- the module instantiation path and its import resolver among them --
# compiles into that file, not into the wasm and not into the runtime bundle. A correct deploy of the
# import-resolver fix was reported "marker MISSING" and scored FAIL by this check.
NJS="$(ls frontend/public/_framework/dotnet.native.*.js 2>/dev/null | head -1)"
fail=0
for m in "$@"; do
	if grep -aqF -- "$m" "$W"; then
		echo "  marker OK      $m (wasm)"
	elif [ -n "$RJS" ] && grep -aqF -- "$m" "$RJS"; then
		echo "  marker OK      $m (runtime js)"
	elif [ -n "$NJS" ] && grep -aqF -- "$m" "$NJS"; then
		echo "  marker OK      $m (native js)"
	else
		echo "  marker MISSING $m"
		fail=1
	fi
done

# ikvmcraft's `make build` ends with split-resources.py (ikvmcraft b4f9da9): every .wasm/.dll over 20 MB becomes
# <name>0..N for Cloudflare Pages' per-file limit, listed in _framework/split-resources.json and loaded through
# split-resources.js. The page fetches that manifest at boot; without this step it gets vite's index.html fallback
# and dies with "Unexpected token '<' ... is not valid JSON" before the runtime starts (2026-10-31: a whole gate
# batch failed at page load after a deploy). After the marker check (which reads the whole wasm), before the vite
# restart below (vite caches the public/ listing).
if [ -f split-resources.py ]; then
	python3 split-resources.py || { echo "deploy: FAIL — split-resources.py" >&2; exit 1; }
	echo "  split OK       split-resources.py -> _framework/split-resources.json"
fi

# AND CHECK IT OVER HTTP, not just on disk.
#
# Everything above inspects the filesystem. But the browser fetches through the vite dev server, and vite
# can hold a stale view of public/: after _framework was deleted and recreated (which this script does on
# every run, and which the failed-publish path leaves half-done), vite served its SPA index.html fallback
# for every _framework asset -- HTTP 200, 429 bytes of HTML, for a 1,056,256-byte DLL that was present on
# disk the whole time. The app then died in mono_download_assets with "Failed to fetch" and integrity
# errors, roughly 200 lines deep in a 517 KB log, nowhere near anything that says "restart the server".
#
# Disk-correct and served-correct are different claims, and this script's whole premise is that only the
# served bytes count. Fix is to restart the dev server; the point here is to say so immediately.
URL_BASE="${SERVE_URL%/}"

# Restart the dev server rather than telling a human to. The stale-public view is not occasional: every run
# of this script deletes and recreates _framework with freshly CONTENT-HASHED names, so vite's cached
# listing is wrong by construction afterwards. Telling the operator to restart it means the next run boots
# a broken app and the failure surfaces as "Failed to fetch" inside the game log instead of here.
#
# The systemd unit is only ONE of the two ways this server gets started, and assuming it was the way cost a
# deploy: the unit was `inactive` while port 5021 was served by a hand-started `npm run dev`, so
# `systemctl restart` succeeded at restarting nothing, the `2>/dev/null` hid that it had nothing to restart,
# and the stale listing survived to fail the dll check at the very bottom of the script. Restart whatever is
# actually listening, and say which mechanism was used -- a silent no-op is the failure mode to avoid here.
VITE_PORT="${URL_BASE##*:}"
restarted=""
# ONE unit name for the dev server, shared with drive/ensure-server.sh: $DEV_UNIT running $DEV_CMD (lib/paths.mjs). Two
# names used to coexist (ikvmcraft-dev / pnpm here-and-there, ikvmcraft-vite / npm here), so this branch missed the live
# unit and killed its process from the one below.
if systemctl --user is-active --quiet "$DEV_UNIT.service" 2>/dev/null; then
	systemctl --user restart "$DEV_UNIT.service" 2>/dev/null
	restarted="systemd unit $DEV_UNIT"
else
	# Whoever holds the port, regardless of how it was launched. Re-launch it the way package.json defines
	# `dev`, detached, so it outlives this script.
	vpid="$(ss -ltnp 2>/dev/null | awk -v p=":$VITE_PORT" '$4 ~ p".*" {match($0, /pid=[0-9]+/); if (RSTART) print substr($0, RSTART+4, RLENGTH-4)}' | head -1)"
	vdir="$APP/frontend"
	if [ -n "$vpid" ] && [ -d "$vdir" ]; then
		kill "$vpid" 2>/dev/null
		for _i in $(seq 1 15); do ss -ltn 2>/dev/null | grep -q ":$VITE_PORT " || break; sleep 1; done
		# `setsid --fork`, not plain `setsid`: without --fork the dev server stays a CHILD of this script, and
		# bash then blocks in do_wait at the end of the run -- observed hanging a deploy for 12 minutes with
		# everything already published, so the serve checks below never ran. --fork double-forks it to init.
		# `disown` is the belt to that braces: nothing left in this shell's job table to wait on.
		# AND its own systemd unit, not just setsid: a deploy run inside a transient service (CLAUDE.md, "Long-running
		# work") has its whole cgroup killed when that service exits, setsid'd children included -- which took the
		# user's dev server down after the R500 deploy. As $DEV_UNIT.service it also takes the branch above next time.
		systemctl --user reset-failed "$DEV_UNIT" >/dev/null 2>&1
		if systemd-run --user --quiet --unit="$DEV_UNIT" --working-directory="$vdir" -- \
			bash -lc "exec $DEV_CMD </dev/null" 2>/dev/null; then
			restarted="hand-started dev server (pid $vpid, relaunched as $DEV_UNIT.service in $vdir)"
		else
			( cd "$vdir" && setsid --fork bash -lc "exec $DEV_CMD" >/tmp/ikvmcraft-dev.log 2>&1 </dev/null & disown ) 2>/dev/null
			restarted="hand-started dev server (pid $vpid, relaunched in $vdir)"
		fi
	fi
fi
if [ -n "$restarted" ]; then
	for _i in $(seq 1 40); do
		curl -sk -o /dev/null --max-time 3 "$URL_BASE/" 2>/dev/null && break
		sleep 1
	done
	echo "  vite restarted via $restarted (stale public/ listing after _framework was recreated)"
else
	echo "  !! nothing restarted: no active unit and nothing listening on $VITE_PORT — the serve checks below" >&2
	echo "     are the only thing standing between you and measuring a stale app." >&2
fi

# Only now act on a marker miss. Exiting at the marker check itself left vite holding its stale listing, so a
# failed verification actively broke the NEXT boot as well ("boot failed", no assets) -- a check that damages
# the thing it is checking. Restart first, report second.
[ "$fail" -eq 0 ] || { echo "deploy: FAIL — the new code is in neither the served wasm nor the runtime js. Do not measure." >&2; exit 1; }

# Verify BOTH the runtime wasm AND a managed DLL.
#
# Checking only the wasm is not enough, learned the hard way: with a stale vite the 189 MB
# dotnet.native.<hash>.wasm served CORRECTLY while every managed DLL fell back to the 413-byte SPA
# index.html. This script printed "served OK" and the app still died in mono_download_assets. The two
# asset classes take different paths through vite, so one says nothing about the other.
check_served () {   # <label> <url-path> <expected-bytes>
	local label="$1" path="$2" want="$3" got
	got=$(curl -sk -o /dev/null -w '%{size_download}' --max-time 60 "$URL_BASE$path" 2>/dev/null)
	if [ -z "$got" ] || [ "$got" = "0" ]; then
		echo "deploy: WARNING could not fetch $URL_BASE$path — is the dev server up?" >&2
		return 0
	fi
	if [ "$got" != "$want" ]; then
		echo "deploy: FAIL — dev server serves $got bytes for a $want byte $label ($path)." >&2
		echo "        vite still has a stale view of public/ even after a restart." >&2
		return 1
	fi
	echo "  served OK      $label $got bytes over HTTP == on disk"
	return 0
}
if [ -f "$W" ]; then
	check_served "wasm" "/_framework/$(basename "$W")" "$(stat -c%s "$W")" || exit 1
else   # split by split-resources.py above: the parts and the manifest are what the page fetches
	check_served "wasm part 0" "/_framework/$(basename "$W")0" "$(stat -c%s "${W}0")" || exit 1
	check_served "split manifest" "/_framework/split-resources.json" "$(stat -c%s frontend/public/_framework/split-resources.json)" || exit 1
fi
DLL="$(ls frontend/public/_framework/IKVM.Runtime.*.dll 2>/dev/null | head -1)"
if [ -n "$DLL" ]; then
	check_served "dll" "/_framework/$(basename "$DLL")" "$(stat -c%s "$DLL")" || exit 1
fi
echo "deploy: ok"

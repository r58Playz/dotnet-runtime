#!/usr/bin/env python3
"""Resolve the SERVER TICK thread's tid in one of our captures, by evidence rather than by rank.

Both busy threads are named `DedicatedWorker`, so --by-thread order does not identify them and picking
the top one silently gives the client. The server tick is the thread that runs Lithium's tick scheduler;
the client is the one that runs realize_glenv. Ask which symbols a tid actually executed.

  usage: servertid.py <capture-dir> [--client]
"""
import re, subprocess, sys, os

d = sys.argv[1]
want_client = "--client" in sys.argv
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
perfraw = paths.tool("perfraw.py")

_r = subprocess.run([sys.executable, perfraw, d, "--phase", "ingame", "--by-thread"],
                    capture_output=True, text=True)
# Propagate perfraw's refusal verbatim. Without this, a void-window capture reports "no
# DedicatedWorker threads", which is true but names the symptom instead of the cause.
if _r.returncode != 0:
    sys.exit((_r.stderr or _r.stdout).strip() or f"perfraw exited {_r.returncode}")
out = _r.stdout
# A thread whose COMM record perf never saw prints as `?` (pz b2, 2026-10-03: the SERVER thread was `?`, so it was not a
# candidate at all and the census refused the run). Unnamed threads are candidates too; the symbol evidence below decides.
busy = [(m.group(2), int(m.group(1))) for m in re.finditer(r"%\s+(\d+)\s+tid=(\d+)\s+(?:DedicatedWorker|\?)\s", out)][:4]
tids = [t for t, _ in busy]
if not tids:
    sys.exit(f"no DedicatedWorker threads in {d}")

# 1.16.1 intermediary names, and 26.x's Mojang names for the same classes (26.x ships unobfuscated): ServerLevel =
# class_3218, LevelTicks = Lithium's/vanilla's tick scheduler, LevelRenderer = class_761 (WorldRenderer).
SERVER = re.compile(r"ServerTickScheduler|net\.minecraft\.class_3218|selectTicks|class_1937:method_8398"
                    r"|net\.minecraft\.server\.level\.ServerLevel:|net\.minecraft\.world\.ticks\.LevelTicks:|ServerChunkCache:")
CLIENT = re.compile(r"wink_present|gex_wgpu|class_761|WorldRenderer|net\.minecraft\.client\.renderer\.LevelRenderer:")

best = None
for t in tids:
    o = subprocess.run([sys.executable, perfraw, d, "--phase", "ingame", "--tid", t],
                       capture_output=True, text=True).stdout
    s, c = len(SERVER.findall(o)), len(CLIENT.findall(o))
    score = (c - s) if want_client else (s - c)
    if best is None or score > best[0]:
        best = (score, t, s, c)

score, tid, s, c = best
# TIER-OFF CAPTURES (R313): with MONO_WASM_JIT_AUTO=0 the server tick runs in the interpreter, so none of the
# SERVER symbols (JIT-emitted method names) exist and the server score is 0 for every thread -- but the client
# is still identifiable by its GL stack. Then the server is the other of the two busy workers. Only taken when
# both are genuinely busy (the other has >= half the client's samples), so a livelocked capture still fails.
if score <= 0 and not want_client and len(busy) >= 2:
    cl = None
    for t in tids:
        o = subprocess.run([sys.executable, perfraw, d, "--phase", "ingame", "--tid", t],
                           capture_output=True, text=True).stdout
        cs = len(CLIENT.findall(o)) - len(SERVER.findall(o))
        if cs > 0 and (cl is None or cs > cl[0]):
            cl = (cs, t)
    if cl:
        n = dict(busy)
        other = next((t for t in tids[:2] if t != cl[1]), None)
        if other and n[other] * 2 >= n[cl[1]]:
            print(f"[servertid] no JIT server symbols; server = busy worker {other} (client {cl[1]} by GL symbols)",
                  file=sys.stderr)
            score, tid = 1, other
# FAIL CLOSED. This used to warn and print the tid anyway, and R269 shows why that is not enough:
# on a livelocked capture it printed a COMPILE WORKER, whose census then read 1,064 M instr/tick and
# 100x native off a thread that never ran a tick. A warning on stderr is invisible inside
# `$(servertid.py ...)`, which is exactly how every caller here uses it.
#
# score <= 0 means no thread in this capture looks more like the wanted one than its opposite -- the
# evidence is absent, not merely thin, so there is no tid to hand back. --force prints the best guess
# for the case where you are deliberately inspecting a broken run.
if score <= 0 and "--force" not in sys.argv:
    sys.exit(f"!! no {'client' if want_client else 'server'} thread identifiable in {d}: "
             f"best candidate tid={tid} has server-hits={s} client-hits={c}.\n"
             f"   A capture whose busy threads run neither the tick scheduler nor the GL stack is "
             f"usually a void/livelocked window -- check phases.json ingameVoid before trusting it.\n"
             f"   Pass --force to print the best guess anyway.")
print(tid)

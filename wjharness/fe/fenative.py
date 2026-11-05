#!/usr/bin/env python3
"""fenative.py <capdir> <server tid> [--event fe_l1i_miss] -- the native (non-wasm) functions behind a front-end event's
samples on one thread, named through perfraw (PERFRAW_DSO_SYMS=1: chromium's .symtab by build-id via debuginfod)."""
import collections, os, sys
os.environ["PERFRAW_DSO_SYMS"] = "1"
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfraw
d, tid = sys.argv[1], int(sys.argv[2])
ev = sys.argv[sys.argv.index("--event") + 1] if "--event" in sys.argv else "fe_l1i_miss"
for e in (ev, "cycles"):
    cap, syms, ph = perfraw.open_capture(d, want_chains=False, event=e)
    t0, t1 = perfraw.phase_window(ph, "ingame")
    s = [ip for t, ip, tm, _ in cap.window(t0, t1) if t == tid]
    c = collections.Counter()
    for ip in s:
        n = syms.lookup(ip) or b"?"
        if n.startswith(b"DSO:"):
            p, _, f = n[4:].partition(b"!")
            c[os.path.basename(p.decode()) + "!" + (f.decode("utf8", "replace")[:90] or "?")] += 1
    print(f"\n== {e}: {len(s)} samples; native {sum(c.values())} ({100 * sum(c.values()) / max(len(s), 1):.1f}%)")
    for k, v in c.most_common(25):
        print(f"  {100 * v / max(len(s), 1):6.2f}%  {k}")

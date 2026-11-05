#!/usr/bin/env python3
"""Per-BUCKET instructions/frame for one of our threads, against native's whole-frame cost.

WHY THIS EXISTS, next to join.py and split.py
---------------------------------------------
`join.py` compares the methods that exist on BOTH sides and `split.py` says how much of our thread is
Java at all. Neither answers the question the lever list is actually built from: *which SUBSYSTEM is the
per-frame cost in, and how does that subsystem compare to native's ENTIRE frame.* A bucket at 11.7 M
instr/frame is 2.2x everything native's client thread does in a frame, and that framing is what stops a
7%-of-thread pool reading as small.

It also reports per TICK, because the SERVER thread is the better instrument for anything in the JIT /
IKVM / mono scope: it carries no GL, no chromium and no Unsafe traffic, so ~99.5% of it is work we can
change, where the client thread is diluted by ~20% of untouchable GL and browser cost.

REQUIRES AN INSTRUCTION-WEIGHTED CAPTURE
----------------------------------------
    MCPERF_EVENT=instructions MCPERF_PERIOD=2000000 node ../mcperf.mjs <out> \
        --warm-ms 180000 --bench-ms 120000 --no-walk
Under a task-clock capture perfraw prints no "G instr" line and this refuses to run, rather than
silently reporting time shares in a column headed M instr/frame. That substitution is exactly how R240's
symbol figures ended up off a superseded census.

VERIFY THE CLASSIFIER BEFORE QUOTING IT
---------------------------------------
Six classifier bugs of the same shape have shipped in this tree, every one of them written from a
plausible mental model of the symbols and never checked against them. So:
  * `--show <bucket-substring>` prints every symbol in a bucket with its M/frame. Read it.
  * `unclassified` is always printed, never folded into `other`. If it is more than a few percent the
    table is not ready to be quoted.
  * The ORDER of the rules below is load-bearing -- `Unsafe` must be tested before the generic IKVM
    rule, and the GL rule before the LWJGL one -- so do not sort them.
"""
import re, subprocess, sys, os, json, collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
PERFRAW = paths.tool("perfraw.py")

# (bucket, regex). FIRST MATCH WINS; see the ordering note above.
RULES = [
    ("chromium binary",             r"^DSO:/usr/lib/chromium"),
    ("other DSO",                   r"^DSO:"),
    ("V8 builtin/trampoline",       r"^Builtin:|wasm-to-js|wasm-function\["),
    # GL is more than the fpe_/gl4es prefixes: realize_textures, bindBuffer and the emscripten-side
    # preDraw* glue all landed in `unclassified` on the first run of this tool.
    # mesa-wink: Mesa's GL frontend (st/mesa, vbo, glthread, NIR, util) and the gex_wgpu Gallium driver, plus Tint and
    # emdawnwebgpu's webgpu.cpp, all in wasm; the WebGPU JS glue lands in `V8 builtin/trampoline` / `libc / JS glue`.
    ("GL (mesa-wink)",              r"JS:(wink_|gex_wgpu|_mesa_|st_|vbo_|glthread|nir_|util_|u_upload|u_primconvert|draw_"
                                    r"|tint::|wgpu[A-Z]|emwgpu|_gl[A-Z]|glDraw)"),
    ("LWJGL bindings",              r"ikvmc_lwjgl3|org\.lwjgl"),
    ("sun.misc.Unsafe emulation",   r"Unsafe"),
    # pthread_mutex_* IS lock traffic and belongs here, not in a residual bucket.
    ("monitors / locks",            r"monitor|ObjectHeader_Try|Monitor_|pthread_mutex"),
    # R225's relabel, encoded so it cannot be re-conflated: `__<>MHC` stubs are IKVM's LAZY-LINKING
    # fallback and are NOT java.lang.invoke. The real MethodHandle machinery is DMH./BMH./LambdaForm/
    # PairwiseConvert, and it gets its own row precisely so the two can never be summed by accident.
    ("java.lang.invoke (real MH)",  r"\bDMH\.|\bBMH\.|LambdaForm|PairwiseConvert|MethodHandle"),
    ("IKVM lazy-link/dyn dispatch", r"__<>MHC|__<>DynamicBinder__|__<>lazyrecompiled|DynamicAaload|__field_"),
    ("Java type-check machinery",   r"InstanceCheck|isinst_with_cache|castclass|DynamicCast|DynamicInstanceOf|CastCheck"),
    ("our JIT tier helpers",        r"JS:(mono_wasm_jit_|wj_|wasm_jit_|get_virtual_method_fast|ldvirtfn|do_jit_call)"),
    ("IKVM runtime (other)",        r"ByteCodeHelper|IKVM_Runtime|IKVM\.Runtime|IKVM_Java_"),
    ("mono runtime C",              r"JS:(mono_|sgen_|mini_|ves_icall|object_new|copy_stack_data|llvmonly"
                                    r"|virt_stelemref|stelemref)"),
    # AOT'd corlib symbols are spelled with UNDERSCORES (System_Collections_Concurrent_...), so a rule
    # keyed on "System." misses every one of them. Both spellings occur in the same capture.
    ("corlib",                      r"JS:corlib_|JS:System_|System\."),
    ("real Java (app+mods)",        r"net\.minecraft|me\.jellysquid|java\.|org\.|net\.fabricmc|it\.unimi|com\.mojang|com\.google"),
    # memcmp/memcpy and the minified dotnet.runtime.js glue are neither ours nor Java; naming them keeps
    # them out of `unclassified`, where they were the two largest entries.
    ("libc / JS glue",              r"JS:(memcmp|memcpy|memmove|memset|strlen)|_framework/dotnet\.(runtime|native)"),
]
RULES = [(b, re.compile(p)) for b, p in RULES]

def classify(sym):
    for b, p in RULES:
        if p.search(sym):
            return b
    return "unclassified"

def census(capture, sel):
    """(M-instr-per-second-of-window scale, [(pct, symbol)]) for one thread."""
    cmd = ["python3", PERFRAW, capture, "--phase", "ingame", "--top", "200000"] + sel
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout
    # PROPAGATE perfraw's OWN refusal instead of guessing at it. This branch used to report every
    # empty result as "task-clock capture, re-record", which is a diagnosis, not an observation --
    # and it was WRONG for the void-window refusal added after R269, sending the reader off to
    # re-record a capture whose correct disposition is to be thrown away. Same shape as the six
    # classifier bugs this file's own header warns about.
    if r.returncode != 0:
        sys.exit((r.stderr or out).strip() or f"perfraw exited {r.returncode} with no message")
    m = re.search(r"([\d.]+) G instr", out)
    if not m:
        sys.exit("no 'G instr' in perfraw output -- this is a task-clock capture, not an "
                 "instruction-weighted one. Re-record with MCPERF_EVENT=instructions MCPERF_PERIOD=2000000.")
    rows = [(float(a), b) for a, b in re.findall(r"\n\s+([\d.]+)%\s+(.*)", out)]
    span = float(re.search(r"= ([\d.]+)s", out).group(1))
    return float(m.group(1)), span, rows

def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__.strip().splitlines()[0] +
                 "\n\nusage: buckets.py <mcperf-outdir> [--marker SYM | --tid N] [--frames N | --ticks]"
                 "\n                   [--native M] [--show BUCKET] [--tsv FILE]")
    cap = a[0]
    def opt(k, d=None):
        return a[a.index(k) + 1] if k in a else d
    sel = (["--marker", opt("--marker")] if opt("--marker") else
           ["--tid", opt("--tid")] if opt("--tid") else ["--marker", "wink_present"])
    G, span, rows = census(cap, sel)

    # DENOMINATOR. Per FRAME for the render thread; per 20 Hz TICK for the server thread, because the
    # server tick is fixed-rate and a per-frame number for it is meaningless (R184).
    if "--ticks" in a:
        # THE SERVER TICK IS NOT ACTUALLY 20 Hz FOR US. Minecraft warns "Running Nms or K ticks behind"
        # and then JUMPS ITS CLOCK FORWARD, so those K ticks are never executed. Dividing by span*20
        # therefore spreads a window's work over ticks that did not happen and UNDERSTATES the per-tick
        # cost -- by 15.6% on the capture this tool was written against (376 of 2406 ticks skipped).
        # Native never hits this at all (zero warnings in every stored native capture), which is itself
        # the finding: their server tick has ~90% headroom and ours has none.
        skipped = 0
        gl = os.path.join(cap, "game.log")
        if os.path.exists(gl):
            lines = open(gl, errors="replace").read().splitlines()
            i0 = next((i for i, l in enumerate(lines) if "[mcw] START" in l), None)
            i1 = next((i for i, l in enumerate(lines) if "[mcw] END" in l), None)
            if i0 is not None and i1 is not None and i1 > i0:
                skipped = sum(int(m.group(1)) for l in lines[i0:i1]
                              for m in [re.search(r"or (\d+) ticks behind", l)] if m)
        denom, unit = span * 20.0 - skipped, "tick"
        if skipped:
            print(f"[server fell {skipped} ticks behind inside the window -- "
                  f"{span * 20.0:.0f} nominal ticks, {denom:.0f} executed]")
    else:
        frames = opt("--frames")
        if frames is None:
            j = json.load(open(os.path.join(cap, "phases.json")))
            frames = (j.get("window") or {}).get("frames")
        if not frames:
            sys.exit("no frame count: pass --frames N (phases.json has none)")
        denom, unit = float(frames), "frame"
    per = G * 1e3 / denom                       # M instr per frame/tick at 100% of the thread
    nat = float(opt("--native", "5.17" if unit == "frame" else "10.61"))

    agg = collections.Counter()
    for p, n in rows:
        agg[classify(n)] += p

    if opt("--show"):
        want = opt("--show").lower()
        sel_rows = [(p, n) for p, n in rows if want in classify(n).lower()]
        print(f"=== symbols in bucket(s) matching {opt('--show')!r}: "
              f"{sum(p for p, _ in sel_rows) / 100 * per:.2f} M instr/{unit}, {len(sel_rows)} symbols ===")
        for p, n in sel_rows:
            print(f"  {p / 100 * per:8.3f} M/{unit}  {n[:110]}")
        return

    print(f"{cap}  thread {' '.join(sel)}")
    print(f"{G:.1f} G instr / {denom:.0f} {unit}s = {per:.1f} M instr/{unit}    "
          f"native = {nat} M instr/{unit}  =>  {per / nat:.1f}x\n")
    print(f"{'M instr/' + unit:>14} {'x native ' + unit:>13} {'% thread':>9}  bucket")
    lines = []
    for k, v in sorted(agg.items(), key=lambda x: -x[1]):
        mi = v / 100 * per
        print(f"{mi:14.2f} {mi / nat:13.2f} {v:8.2f}%  {k}")
        lines.append(f"{mi:.4f}\t{mi / nat:.4f}\t{v:.4f}\t{k}")
    tot = sum(agg.values()) / 100 * per
    print(f"{tot:14.2f} {tot / nat:13.2f} {sum(agg.values()):8.2f}%  TOTAL")
    if agg["unclassified"] / max(sum(agg.values()), 1e-9) > 0.05:
        print("\n!! unclassified > 5% -- run --show unclassified before quoting this table")
    if opt("--tsv"):
        with open(opt("--tsv"), "w") as f:
            f.write(f"# {cap} {' '.join(sel)} {G:.1f}G/{denom:.0f}{unit}s per={per:.4f} native={nat}\n")
            f.write("m_instr_per_%s\tx_native\tpct_thread\tbucket\n" % unit)
            f.write("\n".join(lines) + "\n")
        print(f"\nwrote {opt('--tsv')}")

if __name__ == "__main__":   # importable: j2d/clientbuckets.py reuses RULES/classify
    main()

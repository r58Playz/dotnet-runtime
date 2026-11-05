#!/usr/bin/env python3
"""Per-method join: native HotSpot vs our wasm JIT, on the SAME Java methods.

The comparison this whole exercise exists for. We execute 29.3x more instructions per frame than native
(2026-09-10); this says whether that is UNIFORM across methods (systemic -- the managed execution model)
or CONCENTRATED in a few (specific and fixable). Those imply completely different plans.

Join key: IKVM preserves Java names in CLR metadata, so our symbols carry
`net.minecraft.class_2945:method_12783` inside a mangled signature, and async-profiler emits
`net/minecraft/class_2945.method_12783`. Both normalise to a dotted class.method.

  ours   TSV  : "<self pct of client thread>\\t<symbol>"   (perfraw.py --tsv)
  native      : async-profiler collapsed, -t so frame 0 is the thread name

Usage: join.py <ours.tsv> <ours_total_G> <ours_frames> <native.collapsed> <native_total_G> <native_frames>
                [--thread "Server thread"]
"""
import sys, re, os, collections

ours_tsv, oG, oF, nat_col, nG, nF = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), sys.argv[4], float(sys.argv[5]), int(sys.argv[6])
o_mpf_total = oG * 1e3 / oF          # M instr/frame, whole client thread
n_mpf_total = nG * 1e3 / nF

# --- ours -------------------------------------------------------------------
# Strip the "-<id>-<tier>" suffix FIRST: a bare "turbofan"/"liftoff" match otherwise eats every symbol
# (CLAUDE.md's classifier warning). Then find the Java-ish class:method inside the mangled signature.
JAVA = re.compile(r"([A-Za-z_][A-Za-z0-9_.$]*):([A-Za-z_<][A-Za-z0-9_<>$]*)")
ours = collections.Counter()
for line in open(ours_tsv, errors="replace"):
    p = line.rstrip("\n").split("\t")
    if len(p) < 2: continue
    try: pct = float(p[0])
    except ValueError: continue
    sym = p[1]
    if not sym.startswith("JS:"): continue
    sym = re.sub(r"-\d+-(turbofan|liftoff|builtin)$", "", sym[3:])
    m = JAVA.search(sym)
    if not m: continue
    cls, meth = m.group(1), m.group(2)
    if "." not in cls: continue          # need a package-qualified Java class
    ours[f"{cls}.{meth}"] += pct


def render_tid(nat_col, name="Render thread"):
    """The ONE thread of that name perf counted, resolved from the sibling perf.txt.

    Minecraft names THREE threads "Render thread" (verified in out-prof/threads.txt: 3667897, 3668085,
    3668086) and async-profiler's -t tags each stack with its own. `perf stat` was attached to exactly
    one of them, and its instruction count is the denominator every M/frame here is scaled by. A
    substring match on "Render thread" therefore pooled 21,096 samples under an 18.5 G total that
    belongs to the 18,077 of them -- deflating every native per-method figure by 16.7% and inflating
    every ours-vs-native ratio by ~20%. R240's "15.7x on matched methods" carries that error.

    Returns the tid as a string, or None if perf.txt is absent (then we fall back to the old substring
    behaviour and SAY SO, rather than silently changing what a stored result means).
    """
    pt = os.path.join(os.path.dirname(os.path.abspath(nat_col)), "perf.txt")
    if not os.path.exists(pt):
        return None
    for line in open(pt, errors="replace"):
        m = re.match(r"\s*" + re.escape(name) + r"-(\d+)\s", line)
        if m:
            return m.group(1)
    return None

# --thread picks which side of the workload to compare. The SERVER TICK is the better instrument for
# anything in the JIT / IKVM / mono scope: native's server tick is 94% real Java (9.99 of 10.61
# M instr/tick) and ours carries no GL, no chromium and no Unsafe traffic, where the CLIENT thread is
# diluted on both sides by a GL stack that is out of scope and ~7x native's whole frame on ours.
_tname = sys.argv[sys.argv.index("--thread") + 1] if "--thread" in sys.argv else "Render thread"
_rtid = render_tid(nat_col, _tname)
if _rtid:
    want = lambda f: ("tid=" + _rtid) in f
else:
    sys.stderr.write(f"!! no perf.txt beside the collapsed profile -- pooling every thread "
                     f"named {_tname!r}; see render_tid()\n")
    want = lambda f: _tname in f

# --- native -----------------------------------------------------------------
nat = collections.Counter(); nat_thread_total = 0
for line in open(nat_col, errors="replace"):
    line = line.rstrip("\n")
    i = line.rfind(" ")
    if i < 0: continue
    stack, cnt = line[:i], line[i+1:]
    try: c = int(cnt)
    except ValueError: continue
    frames = stack.split(";")
    if not frames: continue
    if not want(frames[0]): continue                   # -t puts the thread name first
    nat_thread_total += c
    leaf = frames[-1]                                  # SELF time
    leaf = leaf.split("_[")[0].strip()
    leaf = leaf.replace("/", ".")
    leaf = re.sub(r"\.$", "", leaf)
    nat[leaf] += c

if nat_thread_total == 0:
    print(f"NO {_tname} samples in the collapsed profile -- check -t was passed"); sys.exit(1)

rows = []
for k, pct in ours.items():
    o_mpf = pct / 100.0 * o_mpf_total
    nc = nat.get(k, 0)
    n_mpf = nc / nat_thread_total * n_mpf_total
    rows.append((o_mpf, n_mpf, k, nc))
rows.sort(reverse=True)

matched_o = sum(r[0] for r in rows if r[3] > 0)
matched_n = sum(r[1] for r in rows if r[3] > 0)
print(f"ours {o_mpf_total:.1f} M instr/frame total   native {n_mpf_total:.2f}   overall {o_mpf_total/n_mpf_total:.1f}x")
print(f"joined methods: {sum(1 for r in rows if r[3]>0)} of {len(rows)} of ours appear on native")
print(f"joined mass: ours {matched_o:.1f} M/frame, native {matched_n:.2f} M/frame -> {matched_o/matched_n if matched_n else float('nan'):.1f}x on matched methods\n")
print(f"{'method':<52}{'ours M/f':>10}{'nat M/f':>9}{'ratio':>8}")
for o_mpf, n_mpf, k, nc in rows[:30]:
    r = f"{o_mpf/n_mpf:.0f}x" if n_mpf > 0 else "ABSENT"
    print(f"{k[-51:]:<52}{o_mpf:>10.3f}{n_mpf:>9.3f}{r:>8}")

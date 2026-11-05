#!/usr/bin/env python3
"""Split OUR client-thread instructions by whether the method exists on native, and why not."""
import sys, re, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
ours_tsv, oG, oF, nat_col, nG, nF = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), sys.argv[4], float(sys.argv[5]), int(sys.argv[6])
o_tot = oG*1e3/oF; n_tot = nG*1e3/nF
JAVA = re.compile(r"([A-Za-z_][A-Za-z0-9_.$]*):([A-Za-z_<][A-Za-z0-9_<>$]*)")
IKVM = re.compile(r"__<>(DynamicBinder__|MHC\d|lazyrecompiled_|invokeCache)|__field_|__dynamic_|IKVM\.Runtime|IKVM_Runtime|__<Unloadable>")
# Same Render-thread selection as join.py, and for the same reason: THREE threads carry that name and
# perf counted exactly one. See join.py's render_tid() for the size of the error this was causing.
def _render_tid(nat_col):
    pt = os.path.join(os.path.dirname(os.path.abspath(nat_col)), "perf.txt")
    if not os.path.exists(pt): return None
    for line in open(pt, errors="replace"):
        m = re.match(r"\s*Render thread-(\d+)\s", line)
        if m: return m.group(1)
    return None
_rtid = _render_tid(nat_col)
if _rtid:
    want = lambda f: ("tid=" + _rtid) in f
else:
    sys.stderr.write("!! no perf.txt beside the collapsed profile -- pooling every 'Render thread'\n")
    want = lambda f: "Render thread" in f

nat=collections.Counter(); ntot=0
for line in open(nat_col, errors="replace"):
    i=line.rstrip().rfind(" ")
    if i<0: continue
    st,c=line[:i],line[i+1:]
    try: c=int(c)
    except ValueError: continue
    fr=st.split(";")
    if not fr or not want(fr[0]): continue
    ntot+=c
    for f in fr[1:]:                       # WHOLE STACK, not just leaf: a method C2 inlined still
        nat[f.split("_[")[0].replace("/",".")] += 0   # appears as a frame if it appears at all
    nat[fr[-1].split("_[")[0].replace("/",".")] += c
buckets=collections.Counter(); ex=collections.defaultdict(list)
for line in open(ours_tsv, errors="replace"):
    p=line.rstrip("\n").split("\t")
    if len(p)<2: continue
    try: pct=float(p[0])
    except ValueError: continue
    sym=p[1]
    mpf = pct/100.0*o_tot
    if not sym.startswith("JS:"):
        buckets["non-JS (chromium/libc/DSO)"]+=mpf; continue
    s=re.sub(r"-\d+-(turbofan|liftoff|builtin)$","",sym[3:])
    if IKVM.search(s):
        buckets["IKVM machinery (no native counterpart)"]+=mpf; ex["ikvm"].append((mpf,s[:70])); continue
    m=JAVA.search(s)
    if not m or "." not in m.group(1):
        buckets["runtime/JIT helpers + non-Java"]+=mpf; ex["rt"].append((mpf,s[:70])); continue
    k=f"{m.group(1)}.{m.group(2)}"
    if k in nat and nat[k]>0:
        buckets["Java method, EXECUTES on native"]+=mpf; ex["both"].append((mpf,k))
    elif k in nat:
        buckets["Java method, on native stacks but ~0 self (INLINED)"]+=mpf; ex["inl"].append((mpf,k))
    else:
        buckets["Java method, ABSENT from native profile"]+=mpf; ex["abs"].append((mpf,k))
print(f"OURS total {o_tot:.1f} M instr/frame   NATIVE total {n_tot:.2f}   ratio {o_tot/n_tot:.1f}x\n")
tot=sum(buckets.values())
for k,v in buckets.most_common():
    print(f"{k:<52}{v:>9.1f} M/f{100*v/tot:>8.1f}%")
print(f"{'TOTAL':<52}{tot:>9.1f} M/f")
for tag,label in (("inl","INLINED AWAY on native (top)"),("abs","ABSENT from native profile (top)"),("both","EXECUTES on both (top)")):
    print(f"\n-- {label} --")
    for mpf,k in sorted(ex[tag],reverse=True)[:10]: print(f"   {mpf:>7.3f}  {k[-60:]}")

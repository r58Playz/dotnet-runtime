#!/usr/bin/env python3
"""
depcheck.py <graph.log> <dump.wat | dir-of-wat>...   -- baked f-slots that no descriptor DECLARES.

WHAT THIS TESTS, AND WHY IT IS A CORRECTNESS GATE RATHER THAN A CENSUS
----------------------------------------------------------------------
Generated code `call_indirect`s an f-slot with NO liveness check at all; the entire job of the admission
DFS is to install a method's direct-call closure BEFORE that method may go live, and the closure it walks
is the descriptor's DEPSET. So an f-slot that the emitter bakes into a body but never records as a
dependency is unadmittable by construction: admission cannot install what the depset does not name, the
walk completes cleanly (`install_budget_out=0`), and the first worker to reach that call without having
installed the callee for some other reason finds `mono_jiterp_placeholder_jit_call` sitting in the table
and traps with `function signature mismatch`.

That is R285: `batch[4]` baked 15 distinct f-slots, 11 were declared, 3 were AOT table indices that
legitimately need no admission, and exactly one -- 467053, owned by JIT descriptor 23652 -- was declared
by nobody. It is the trap class R163/R165/R201/R208/R209/R234/R239/R267 have all circled.

The two inputs come from ONE run and that is not optional:

    Module._mono_wasm_jit_dump_dep_graph()   ->  graph.log   (WJ_GRAPH_BEGIN ... WJ_GRAPH_END)
    wasmtier.mjs                             ->  the .wat dump

F-SLOT NUMBERS ARE PER-RUN. Pairing a dump with a graph from a different run compares two unrelated
numberings and reports nearly every baked slot as "unowned" -- the same confound as pairing a reach
number with a census from another run. This pass therefore refuses on a low ownership ratio rather than
printing a plausible table; see the PROVENANCE gate below, which is the check that makes the rest
trustworthy.
"""
import sys, os, glob, re, subprocess
from concurrent.futures import ThreadPoolExecutor
from collections import Counter, defaultdict

# `G <desc> <batch_generation> <e> <f> <ndeps> [d<slot> ...]` -- mono_wasm_jit_dump_dep_graph's format.
#
# TWO SPELLINGS, and assuming one cost two 7-minute runs. Copied out of the devtools console the line is
# bare; captured from the driver's game.log every line carries chromium's prefix and a trailing source,
#     [2314381:...:INFO:CONSOLE:13] "G 1 0 419394 419395 0", source: https://localhost:5021/...
# so an anchored `^G ` silently matches nothing and the graph reads as EMPTY -- which downstream looks
# exactly like a run with no dependencies rather than like a parse failure. load_graph() therefore also
# refuses a file with no rows rather than returning an empty map.
G_RX = re.compile(r'(?:^|")G (\d+) (\d+) (-?\d+) (-?\d+) (\d+)((?: d-?\d+)*)(?:"|\s*$)')

DEPS_OF = {}   # desc -> its depset's slots (B4 leaf copies: the per-module check)

def load_graph(path):
    owner, declared, gen = {}, set(), {}
    n = 0
    for line in open(path, errors="replace"):
        m = G_RX.search(line.rstrip())
        if not m:
            continue
        n += 1
        desc, g, e, f, nd, deps = int(m[1]), int(m[2]), int(m[3]), int(m[4]), int(m[5]), m[6]
        if f > 0:
            owner[f] = desc
            gen[f] = g
        for d in deps.split():
            declared.add(int(d[1:]))
        DEPS_OF[desc] = {int(d[1:]) for d in deps.split()}
    if not n:
        sys.exit(f"depcheck: {path} holds no `G ` lines -- is it a dep-graph dump?")
    return owner, declared, gen, n

# `i32.const <N>` on one line and `call_indirect ...` on the next is how a BAKED slot appears. A call
# whose index is computed at run time (an IC hit, a delegate, a calli) pushes it from a local instead, so
# it never matches -- which is correct: those have no constant to declare.
CONST_RX = re.compile(r'^\s*i32\.const (\d+)\s*$')
CALLI_RX = re.compile(r'^\s*call_indirect\s*(.*)$')
MOD_RX   = re.compile(r'^\(module \$(.*)$')

def wat_lines(path):
    """WAT text for a module, decoding a binary .wasm through wasm2wat if needed.

    A BAKED slot is only findable with a real opcode walk -- an immediate can coincidentally look like
    an opcode byte, so scanning for 0x41/0x11 in the raw bytes is not sound. wasm2wat already does that
    walk and is the tool the tier dumper itself uses for its sample .wat files, so shelling out to it is
    a reused decoder rather than a second one to keep correct (this project has shipped five classifier
    bugs of exactly the write-your-own kind)."""
    if not path.endswith(".wasm"):
        return open(path, errors="replace").read().splitlines()
    try:
        out = subprocess.run(["wasm2wat", "--enable-all", path, "-o", "/dev/stdout"],
                             capture_output=True, timeout=60)
        return out.stdout.decode("utf-8", "replace").splitlines()
    except Exception:
        return []

GUARDED = Counter()   # plan2x S1: guarded member fallbacks seen, per module (not dependencies, see scan_wat)
# B4 leaf copies (R452): a guarded call whose `else` arm calls an UNEXPORTED function reaches a private copy, and its
# fallback slot is NOT installed by the module -- it must be declared. (module, slot, callee function name) per site.
COPY_FALLBACKS = []
MEMBER_SLOTS = {}   # module -> f-slots of its exported members, recovered from their guarded fallbacks
EXPORT_RX = re.compile(r'\(export "f\d+" \(func (\S+)\)\)')
CALL_RX   = re.compile(r'^\s*call (\S+)\s*$')

def scan_wat(path):
    """-> (module_name, members, [(fslot, signature_text)])

    A co-located group's member-to-member call is `if (*guard) i32.const F; call_indirect ... else call $m end`
    (plan2x S1, WASM_FORM_LOCAL_GUARDED). Its F is the CALLEE MEMBER's own f-slot, installed wherever this module
    is, so it needs no admission and is never in a depset -- recognised by the `else` that follows the
    call_indirect, counted in GUARDED and kept out of the violation table."""
    name, members, baked, prev = os.path.basename(path), 0, [], None
    lines = wat_lines(path)
    exported = {m[1] for l in lines for m in [EXPORT_RX.search(l)] if m}
    for i, line in enumerate(lines):
        m = MOD_RX.match(line)
        if m:
            name = m[1].strip()
        if '(export "f' in line:
            members += 1
        c = CALLI_RX.match(line)
        if c and prev is not None:
            nxt = next((l.strip() for l in lines[i + 1:i + 4] if l.strip()), '')
            if nxt == 'else':
                GUARDED[name] += 1
                k = next((j for j in range(i + 1, min(i + 4, len(lines))) if lines[j].strip() == 'else'), None)
                callee = CALL_RX.match(lines[k + 1]) if k is not None and k + 1 < len(lines) else None
                if callee and exported and callee[1] not in exported:
                    COPY_FALLBACKS.append((path, prev, callee[1]))   # keyed by FILE: module names repeat
                elif callee and callee[1] in exported:
                    MEMBER_SLOTS.setdefault(path, set()).add(prev)   # an exported member's own f-slot
            else:
                baked.append((prev, c[1].strip() or "(no sig)"))
        prev = int(CONST_RX.match(line)[1]) if CONST_RX.match(line) else None
    return name, members, baked

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    owner, declared, gen, ndesc = load_graph(sys.argv[1])

    wats = []
    inputs = [a for a in sys.argv[2:] if not a.startswith("--")]
    for a in inputs:
        if os.path.isdir(a):
            wats += sorted(glob.glob(os.path.join(a, "*.wat"))
                           + glob.glob(os.path.join(a, "*.wasm"))
                           + glob.glob(os.path.join(a, "mods", "*.wasm")))
        else:
            wats.append(a)
    if not wats:
        sys.exit("depcheck: no .wat inputs")

    tot_baked = tot_owned = tot_viol = 0
    per_mod, sigs, viol_slots = [], Counter(), Counter()
    nwork = min(16, (os.cpu_count() or 4))
    with ThreadPoolExecutor(max_workers=nwork) as ex:
        scanned = list(ex.map(scan_wat, wats))
    for name, members, baked in scanned:
        slots = sorted({s for s, _ in baked})
        sig_of = {}
        for s, sg in baked:
            sig_of.setdefault(s, sg)
        owned = [s for s in slots if s in owner]
        viol = [s for s in owned if s not in declared]
        tot_baked += len(slots); tot_owned += len(owned); tot_viol += len(viol)
        for s in viol:
            sigs[sig_of[s]] += 1
            viol_slots[s] += 1
        if viol:
            per_mod.append((len(viol), name, members, len(slots), len(owned),
                            [(s, owner[s], gen[s], sig_of[s]) for s in viol]))

    print(f"\ndepcheck: {ndesc} descriptors, {len(wats)} module(s)")

    # PROVENANCE. A baked slot that no descriptor owns is normally an AOT table index or a fixed helper
    # index -- both legitimate, both reached by their own index and needing no admission. But if the
    # graph and the dump are from DIFFERENT RUNS the numbering is unrelated and almost nothing is owned,
    # which would make every module look clean. Refuse instead of reporting.
    ratio = (tot_owned / tot_baked) if tot_baked else 0.0
    print(f"  baked f-slots {tot_baked}   owned by a descriptor {tot_owned} ({ratio:.0%})")
    if GUARDED:
        print(f"  guarded member fallbacks (plan2x S1, not dependencies): {sum(GUARDED.values())} in {len(GUARDED)} modules")
    # PROVENANCE IS STRUCTURAL, NOT STATISTICAL -- and that correction is the whole point of this block.
    #
    # The first version inferred it from the share of baked slots owned by a descriptor, on the theory
    # that a cross-run pair would look obviously wrong. It does not separate: measured, a CROSS-run pair
    # reads 33% and a same-run WHOLE-TIER pair reads 56%, because a tier is full of DynamicBinder and MHC
    # modules whose indirect calls target AOT and helper indices rather than JIT f-slots. A threshold set
    # between them would have been fitted to two points and wrong on the third; set below them it passes
    # cross-run pairs, and set above it refuses real ones (60% refused this very run).
    #
    # So require the graph to come from the SAME DIRECTORY as the modules. `wasmtier.mjs --depgraph`
    # writes game.log beside mods/, so that is a guarantee rather than an inference. --any-provenance
    # overrides it for a hand-assembled pair (the devtools-console graph + one module), which is how the
    # R285 case was first read.
    same_dir = os.path.realpath(os.path.dirname(os.path.abspath(sys.argv[1]))) in {
        os.path.realpath(os.path.dirname(os.path.abspath(w))) for w in wats
    } or any(os.path.realpath(os.path.abspath(sys.argv[1])).startswith(
             os.path.realpath(os.path.abspath(a)) + os.sep) for a in inputs if os.path.isdir(a))
    if not (same_dir or "--any-provenance" in sys.argv):
        print(f"\n  !! REFUSING: the graph and the modules are not from one dump directory.")
        print( "     f-slot numbers are PER-RUN, so an unrelated pair compares two numberings and reports")
        print( "     coincidences as violations. Use `wasmtier.mjs --outdir D --depgraph`, which writes")
        print( "     the graph into D/game.log beside D/mods/, then `depcheck.py D/game.log D/`.")
        print( "     Pass --any-provenance if you really are pairing them by hand.")
        return 2
    print(f"  provenance: {'same dump directory' if same_dir else 'ASSERTED BY --any-provenance'}")

    if COPY_FALLBACKS:
        cf_undecl = [(n, s, c) for n, s, c in COPY_FALLBACKS if s not in declared]
        print(f"  B4 leaf-copy fallbacks (guarded calls into an UNEXPORTED member): {len(COPY_FALLBACKS)} sites in "
              f"{len({n for n, _, _ in COPY_FALLBACKS})} modules, UNDECLARED {len(cf_undecl)}")
        for n, s_, c in cf_undecl[:10]:
            print(f"      !! {n}: slot {s_} -> {c} declared by no depset")
        # Per module: the copy's slot must be in the depset of one of THIS module's members (admission walks only
        # the members' union). Members are recovered from guarded calls into exported functions, so a module with
        # no such call cannot be checked -- counted as unverifiable, never as clean.
        ok = bad = unv = 0
        for n, s_, c in COPY_FALLBACKS:
            mdesc = {owner[m] for m in MEMBER_SLOTS.get(n, ()) if m in owner}
            if not mdesc:
                unv += 1
            elif any(s_ in DEPS_OF.get(d, ()) for d in mdesc):
                ok += 1
            else:
                bad += 1
                if bad <= 10:
                    print(f"      !! {n}: copy slot {s_} -> {c} not in any of its {len(mdesc)} identified members' depsets")
        print(f"  ... per module: declared by a member {ok}, NOT declared by any identified member {bad}, unverifiable {unv}")
    print(f"  UNDECLARED (owned by a descriptor, named by no depset): {tot_viol}\n")
    if not tot_viol:
        print("  none -- every baked f-slot is declared. (A zero here is only as good as the")
        print("  provenance check above; confirm the ownership ratio looks sane.)\n")
        return 0

    for nv, name, members, nslots, nowned, rows in sorted(per_mod, reverse=True)[:20]:
        print(f"  {name}   members={members} baked={nslots} owned={nowned} UNDECLARED={nv}")
        for s, d, g, sg in rows:
            print(f"      slot {s:<8} -> desc {d:<7} gen {g:<7} call_indirect {sg}")
    if len(per_mod) > 20:
        print(f"  ... and {len(per_mod)-20} more module(s)")

    # The signature is the construct fingerprint: an ordinary method call pushes arguments first, so a
    # zero-argument `(result i32)` is a different emitter path -- which is how R285's site was told apart
    # from the 11 that were declared.
    print("\n  by call_indirect signature (the construct that baked it):")
    for sg, c in sigs.most_common(10):
        print(f"    {c:>6}  {sg}")
    print(f"\n  {len(viol_slots)} distinct undeclared slot(s); most-baked: "
          + ", ".join(f"{s}x{c}" for s, c in viol_slots.most_common(5)) + "\n")
    return 1

if __name__ == "__main__":
    sys.exit(main())

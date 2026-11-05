#!/usr/bin/env python3
"""Re-index the JS counter mirror after WJC_* entries are deleted from the C enum.

The mirror in ikvmcraft/frontend/src/dotnet/jitbench.ts is BY INDEX, and the C->JS name mapping is NOT
mechanical (WJC_DEVIRT_ARM2_EMITTED is `Arm2Emitted`), so it cannot be regenerated from the C names.
What IS reliable is the old index: this reads the enum from a git revision of mini-wasm.h to learn
old-index -> WJC name, reads the enum from the working tree to learn WJC name -> new index, and rewrites
each `Name: <n>` in the mirror accordingly. A mirror entry whose C counter was deleted is reported and
must be removed by hand (deleting it here would also delete its comment, which carries the reason the
counter existed).

  usage: wjcsync.py <old-git-rev>            # report only
         wjcsync.py <old-git-rev> --apply    # rewrite jitbench.ts

NOT IDEMPOTENT -- DO NOT RE-RUN WITH THE SAME <old-git-rev> AFTER AN --apply. The mapping is
old-index -> name -> new-index, read from a git revision on one side and the WORKING TREE on the other.
Once --apply has rewritten a mirror, its indices ARE the new ones, so a second run with the same old rev
reinterprets them as old indices and remaps garbage. R245 hit this: the second run reported
`noRec: 67 (was WJC_ADMIT_DEFERRED)` -- noRec is simply what now sits at 67 -- and only the
"refusing to rewrite while dead entries remain" check stopped it writing. That check is doing double duty
as the idempotency guard, which it should not have to; if you need to re-sync, pass the rev you last
applied FROM, or commit first and use HEAD.

APPEND-ONLY CHANGES NEED NO RUN AT ALL. Counters added immediately before WJC_MAX shift nothing, so the
mirrors stay correct and only the new names need adding by hand (this tool cannot invent them: the
C -> JS name mapping is not mechanical).
"""
import re, subprocess, sys, os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402
REPO = paths.REPO
HDR  = "src/mono/mono/mini/mini-wasm.h"
# THERE ARE TWO BY-INDEX MIRRORS, and forgetting the second is how a "fixed" mirror still reads zeros.
#   jitbench.ts   the in-page dump (the app)
#   lib/mcdrive.mjs  the harness's own delta table, which is what lands in mc-results.jsonl
# They are independent copies of the same enum and have drifted apart before.
MIRRORS = [
    os.path.join(paths.need("IKVMCRAFT"), "frontend/src/dotnet/jitbench.ts"),
    os.path.join(paths.HARNESS, "lib/mcdrive.mjs"),
]

def enum_order(text):
    """Ordered WJC_* names between `WJC_REGISTERED` (the first) and WJC_MAX."""
    # anchor on the enum itself: WJC_MAX is also named in the prose above it
    j = text.index("\nenum {")
    body = text[j:text.index("WJC_MAX", j)]
    # strip comments so a WJC_ name mentioned in prose is not counted
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    names, seen = [], set()
    for m in re.finditer(r"\bWJC_[A-Z0-9_]+\b", body):
        n = m.group(0)
        if n not in seen:
            seen.add(n); names.append(n)
    return names

old = enum_order(subprocess.run(["git", "-C", REPO, "show", f"{sys.argv[1]}:{HDR}"],
                                capture_output=True, text=True, check=True).stdout)
new = enum_order(open(os.path.join(REPO, HDR)).read())
new_idx = {n: i for i, n in enumerate(new)}

# C-side counter RENAMES (old enum name -> new): a rename is not a deletion, and matching purely on
# index would report it as one.
RENAMES = {"WJC_REEMIT_REREGISTER": "WJC_FSLOT_REREGISTER"}
old = [RENAMES.get(n, n) for n in old]

# The two mirrors can be at DIFFERENT epochs -- one may already have been re-indexed against a later
# revision than the other -- so `--only <basename>` restricts a run to the file whose base `old-rev`
# actually is. Running both against one rev when they disagree double-shifts the already-current one,
# and it reports as a pile of dead entries rather than as an error.
only = None
for i, a in enumerate(sys.argv):
    if a == "--only" and i + 1 < len(sys.argv):
        only = sys.argv[i + 1]

any_dead = False
rewrites = []
for JS in MIRRORS:
    if only and os.path.basename(JS) != only:
        continue
    js = open(JS).read()
    # `Name: <n>` inside the counter table only. A trailing comma is required, which is what keeps this
    # off prose like "R179: 17,343 enqueues" -- but see the guard below, which is the real defence.
    # The terminator may be `,` OR `}` -- the LAST entry of a table has no trailing comma, and missing
    # it silently leaves one counter pointing at whatever now sits at its old index. That is exactly the
    # failure this whole tool exists to prevent, and it bit on the first real use.
    entries = list(re.finditer(r"\b([A-Za-z][A-Za-z0-9]*)\s*:\s*(\d+)\s*(?=[,}])", js))
    dead, moved, out, last = [], 0, [], 0
    for m in entries:
        name, i = m.group(1), int(m.group(2))
        if i >= len(old):
            # NOT a counter -- prose, a port number, a source line reference. Leave it alone rather
            # than reporting it: an index past the enum cannot be a mirror entry.
            continue
        c = old[i]
        if c not in new_idx:
            dead.append((name, i, c)); continue
        ni = new_idx[c]
        if ni != i:
            moved += 1
            out.append(js[last:m.start()] + f"{name}: {ni}")
            last = m.end()
    name_only = os.path.basename(JS)
    if dead:
        any_dead = True
        print(f"DEAD entries in {name_only} -- delete these by hand, with their comments:")
        for n, i, c in dead: print(f"  {n}: {i}   (was {c})")
    print(f"{name_only}: {moved} of {len(entries)} entries move")
    rewrites.append((JS, "".join(out) + js[last:]))

print(f"C enum {len(old)} -> {len(new)}")

if "--apply" in sys.argv:
    if any_dead:
        sys.exit("refusing to rewrite while dead entries remain")
    for JS, text in rewrites:
        open(JS, "w").write(text)
        print("rewrote", os.path.basename(JS))

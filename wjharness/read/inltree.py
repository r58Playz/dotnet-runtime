#!/usr/bin/env python3
"""inltree.py <run-N.log> [--all] -- rebuild each verbose compile's inline tree from mono's INLINE START / END / ABORTED
lines (method-to-ir.c:5968/6093/6195; the printed caller is always the ROOT, so nesting comes from the stack) and print,
per compile, every ABORTED inline with its cost and the subtree it had built before refusing. --all prints accepted
subtrees too. `INLINE COLD <root> -> <callee>` (MONO_WASM_JIT_INLINE_COLD_CALL) is a call site in a throw block that was
deliberately left a call: a leaf marked [COLD]. Input: a run log with MONO_VERBOSE_METHOD=<root>;<root>;... (mini.c),
which sets verbose_level 4 for those compiles only. Promoted from scratchpad/runs/2026-11-05-costs."""
import re, sys

log = sys.argv[1]; ALL = '--all' in sys.argv
start_re = re.compile(r'INLINE START (?:0x)?[0-9a-f]+ (.+?) -> (.+)$')
end_re = re.compile(r'INLINE END (.+?) -> (.+)$')
cold_re = re.compile(r'INLINE COLD (.+?) -> (.+)$')
abort_re = re.compile(r'INLINE ABORTED (.+?) \(cost (-?\d+)\)(.*)$')
conv_re = re.compile(r'converting (?:llvm |gsharedvt |gshared |interp only )*method (.+)$')   # mini.c:3882

def short(n):
    n = re.sub(r'\s*\(.*$', '', n)
    return n.split(' ')[-1]

class Node:
    def __init__(s, name): s.name, s.kids, s.cost, s.ok = name, [], None, None

compiles = []; root = None; stack = []
for raw in open(log, errors='replace'):
    k = raw.rfind('", source: ')
    line = raw[:k] if k >= 0 else raw.rstrip('\n')
    i = line.find('"'); line = line[i + 1:] if i >= 0 else line
    m = conv_re.search(line)
    if m:
        root = Node(short(m.group(1))); compiles.append(root); stack = [root]; continue
    if root is None: continue
    m = start_re.search(line)
    if m:
        n = Node(short(m.group(2))); stack[-1].kids.append(n); stack.append(n); continue
    m = cold_re.search(line)
    if m:
        n = Node(short(m.group(2))); n.ok = 'cold'; stack[-1].kids.append(n); continue
    m = end_re.search(line)
    if m and len(stack) > 1:
        stack[-1].ok = True; stack.pop(); continue
    m = abort_re.search(line)
    if m and len(stack) > 1:
        stack[-1].ok = False; stack[-1].cost = int(m.group(2)); stack.pop(); continue

def show(n, d, out):
    tag = 'COLD' if n.ok == 'cold' else 'OK' if n.ok else (f'REFUSED cost {n.cost}' if n.ok is False else '?')
    out.append(f"{'  ' * d}{n.name[-90:]}  [{tag}]")
    for c in n.kids: show(c, d + 1, out)

for c in compiles:
    refused = [n for n in c.kids if n.ok is False]
    def walk(n):
        for k in n.kids:
            if k.ok is False: refused.append(k)
            walk(k)
    refused = []; walk(c)
    print(f'== compile of {c.name}: {sum(1 for _ in c.kids)} top-level inline attempts, {len(refused)} refused')
    for r in refused:
        if r.cost is not None and r.cost >= 60:
            out = []; show(r, 1, out); print('\n'.join(out[:40]))
            if len(out) > 40: print(f'    ... {len(out) - 40} more lines')
    if ALL:
        out = []; show(c, 0, out); print('\n'.join(out))

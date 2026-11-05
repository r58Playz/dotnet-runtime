#!/usr/bin/env python3
"""capture.py <game.log> <outdir> -- extract MONO_WASM_JIT_CAPTURE modules from a run log.

Lines (tab-separated, possibly interleaved across threads, possibly prefixed by the driver's log decoration):
  WASM_JIT_CAPTURE_BEGIN <id> <kind> <len> <name>
  WASM_JIT_CAPTURE <id> <seq> <base64>
  WASM_JIT_CAPTURE_END <id>
Writes <outdir>/mods/<sha16>.wasm (deduplicated by content) and <outdir>/capture.tsv
(sha16, kind, len, copies, name). Also writes <outdir>/tags.tsv from WASM_JIT_ORIGIN_TAG lines.
Reports incomplete captures (BEGIN without END, missing chunks, length mismatch) -- a non-zero count means lines
were dropped between the runtime and the log, and the ledger must not claim completeness."""
import base64, collections, hashlib, os, re, sys

log, out = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(out, 'mods'), exist_ok=True)
beg = {}
chunks = collections.defaultdict(dict)
ended = set()
tags = {}
rb = re.compile(r'WASM_JIT_CAPTURE_BEGIN\t(\d+)\t(\S+)\t(\d+)\t(.*)$')
rc = re.compile(r'WASM_JIT_CAPTURE\t(\d+)\t(\d+)\t([A-Za-z0-9+/=]*)')
re_ = re.compile(r'WASM_JIT_CAPTURE_END\t(\d+)')
rt = re.compile(r'WASM_JIT_ORIGIN_TAG (\d+) (\S+)')
with open(log, 'r', errors='replace') as f:
    for line in f:
        if 'WASM_JIT_' not in line:
            continue
        line = line.rstrip('\n')
        # chromium's console sink wraps each message as `"<message>", source: <url> (<line>)`
        k = line.rfind('", source: ')
        if k >= 0:
            line = line[:k]
        m = rc.search(line)
        if m:
            chunks[int(m.group(1))][int(m.group(2))] = m.group(3); continue
        m = rb.search(line)
        if m:
            beg[int(m.group(1))] = (m.group(2), int(m.group(3)), m.group(4).strip()); continue
        m = re_.search(line)
        if m:
            ended.add(int(m.group(1))); continue
        m = rt.search(line)
        if m:
            tags[int(m.group(1))] = m.group(2)

idx = {}
copies = collections.Counter()
bad = collections.Counter()
for cid, (kind, ln, nm) in beg.items():
    if cid not in ended:
        bad['no_end'] += 1; continue
    ch = chunks.get(cid, {})
    if not ch or sorted(ch) != list(range(len(ch))):
        bad['missing_chunk'] += 1; continue
    data = base64.b64decode(''.join(ch[k] for k in range(len(ch))))
    if len(data) != ln:
        bad['len_mismatch'] += 1; continue
    sha = hashlib.sha256(data).hexdigest()[:16]
    copies[sha] += 1
    if sha not in idx:
        idx[sha] = (kind, ln, nm)
        p = os.path.join(out, 'mods', sha + '.wasm')
        if not os.path.exists(p):
            open(p, 'wb').write(data)
with open(os.path.join(out, 'capture.tsv'), 'w') as f:
    for sha, (kind, ln, nm) in idx.items():
        f.write(f'{sha}\t{kind}\t{ln}\t{copies[sha]}\t{nm}\n')
if tags:
    with open(os.path.join(out, 'tags.tsv'), 'w') as f:
        for t in sorted(tags):
            f.write(f'{t}\t{tags[t]}\n')
print(f'captures {len(beg)} -> {len(idx)} distinct modules, {sum(v[1] for v in idx.values())/1e6:.1f} MB; '
      f'incomplete {dict(bad) or 0}; tags {len(tags)}')
if bad:
    sys.exit(2)

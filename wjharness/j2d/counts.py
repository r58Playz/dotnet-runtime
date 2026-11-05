#!/usr/bin/env python3
"""j2d/counts.py -- per-step hardware counts of the BENCH thread inside a run's window (j2d/run.mjs output).

  counts.py <rundir> [<rundir> ...] [--json]

For each run: the busiest thread by cycles samples inside window.json's window (the bench's batches 2..12 = 2200
steps), then every event's samples x its fixed period for that thread inside the window, per step. Also the THROTTLE
record count (kernel.perf_event_max_sample_rate is 3000/s here; a throttled event undercounts) and the thread's
share of all cycles samples in the window (a check that the bench is one thread).
Prints one line per run; --json writes <rundir>/counts.json."""
import collections, json, mmap, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfread

NAMES = {   # perf evlist names -> short keys
    'instructions': 'instructions', 'cycles': 'cycles',
    'mem_load_retired.l1_miss': 'l1_miss', 'mem_load_retired.l2_miss': 'l2_miss', 'mem_load_retired.l3_miss': 'l3_miss',
    'br_inst_retired.near_call': 'near_call', 'mem_inst_retired.all_loads': 'loads', 'mem_inst_retired.all_stores': 'stores',
    'dtlb_load_misses.walk_active': 'dtlb_walk_cycles', 'dtlb_load_misses.walk_completed': 'dtlb_walks',
    'br_inst_retired.indirect': 'indirect_br', 'icache_data.stalls': 'icache_stall_cycles',
}


def key_of(name):
    for k, v in NAMES.items():
        if k in name:
            return v
    return name


def throttles(path, at, tid, t0, t1):
    """THROTTLE records (type 5) of the bench thread inside the window, per attr index. A throttled event stops
    sampling until the next tick, so its samples x period UNDERCOUNTS: any non-zero entry voids that event's count.
    Record body: time, id, stream_id, then sample_id_all (pid/tid first, as these captures sample TID)."""
    id2a = {}
    for i, x in enumerate(at):
        for j in x['ids']:
            id2a[j] = i
    f = open(path, 'rb'); mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    off, size = struct.unpack_from('<QQ', mm, 40)
    o, end = off, off + size
    out = collections.Counter()
    a0, a1 = int(t0 * 1e9), int(t1 * 1e9)
    while o < end:
        typ, _m, sz = struct.unpack_from('<IHH', mm, o)
        if sz == 0:
            break
        if typ == 5:
            t, ident, _sid = struct.unpack_from('<QQQ', mm, o + 8)
            _pid, rtid = struct.unpack_from('<II', mm, o + 32)
            if rtid == tid and a0 <= t <= a1:
                out[id2a.get(ident, -1)] += 1
        o += sz
    return out


def run_counts(rundir):
    w = json.load(open(os.path.join(rundir, 'window.json')))
    if not w.get('ok') or not w.get('window'):
        return dict(rundir=rundir, ok=False)
    t0, t1 = w['window']
    data = os.path.join(rundir, 'perf.data')
    at = perfread.attrs(data)
    keys = [key_of(x['name']) for x in at]
    cyc_ai = keys.index('cycles')
    by_tid = collections.Counter()
    per = collections.defaultdict(collections.Counter)    # tid -> attr -> samples
    for ai, ip, t, tid, chain, lbr in perfread.samples(data, t0=t0, t1=t1, want_chain=False, want_lbr=False):
        per[tid][ai] += 1
        if ai == cyc_ai:
            by_tid[tid] += 1
    tid, n = by_tid.most_common(1)[0]
    steps = w['steps']
    out = dict(rundir=rundir, arm=w['arm'], set=w['set'], ok=True, tid=tid, steps=steps, span_s=t1 - t0,
               thread_share=n / sum(by_tid.values()), ms_per_step=w['window_ms_per_step'],
               mean_ms_per_step=w['mean_ms_per_step'], checksum=w['checksum'], throttles={},
               per_step={}, samples={})
    thr = throttles(data, at, tid, t0, t1)
    for ai, x in enumerate(at):
        out['per_step'][keys[ai]] = per[tid][ai] * x['period'] / steps
        if thr.get(ai):
            out['throttles'][keys[ai]] = thr[ai]
        out['samples'][keys[ai]] = per[tid][ai]
    # the bench's own clock against the counted cycles: cycles / (window seconds) = effective GHz of that thread
    out['ghz'] = out['per_step']['cycles'] * steps / (t1 - t0) / 1e9
    return out


def main():
    a = sys.argv[1:]
    js = '--json' in a
    for d in [x for x in a if not x.startswith('--')]:
        c = run_counts(d)
        if not c['ok']:
            print(f'{d}: not ok'); continue
        ps = c['per_step']
        ipc = ps['instructions'] / ps['cycles']
        extra = ' '.join(f'{k}={v/1e3:.1f}k' for k, v in ps.items() if k not in ('instructions', 'cycles', 'dummy:u'))
        print(f"{os.path.basename(d):12s} {c['ms_per_step']:.4f} ms/step  instr {ps['instructions']/1e6:.3f} M  "
              f"cyc {ps['cycles']/1e6:.3f} M  IPC {ipc:.2f}  {c['ghz']:.2f} GHz  thr {c['thread_share']:.0%}  "
              f"throttled {c['throttles'] or 0}  {extra}")
        if js:
            json.dump(c, open(os.path.join(d, 'counts.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()

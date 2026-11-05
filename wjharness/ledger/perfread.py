#!/usr/bin/env python3
"""perfread.py -- raw perf.data reader for the gap ledger: per-sample IP, time, tid, callchain AND branch stack (LBR).

samples(path, tid=None, t0=None, t1=None) yields (attr_index, ip, time_ns, tid, callchain_tuple, lbr) where lbr is a
list of (from, to, cycles, type) newest-first, or None when the attr does not sample branches.
attrs(path) -> [dict(type, config, period, stype, bstype, freq, precise, ids, name)]
mmaps(path) -> [(pid, start, end, pgoff, filename)]

Field order is perf_event.h's canonical PERF_SAMPLE_* order; only the fields mcperf captures are supported and
anything else is refused rather than mis-parsed (the R? perfraw lesson: a shifted field reads as plausible data)."""
import mmap, struct, subprocess, sys

S_IDENTIFIER, S_IP, S_TID, S_TIME, S_ADDR, S_READ, S_CALLCHAIN, S_ID, S_CPU, S_PERIOD, S_STREAM_ID, S_RAW, S_BRANCH = \
    1 << 16, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048
S_WEIGHT, S_DATA_SRC, S_WEIGHT_STRUCT = 1 << 14, 1 << 15, 1 << 24
SUPPORTED = S_IDENTIFIER | S_IP | S_TID | S_TIME | S_CALLCHAIN | S_PERIOD | S_BRANCH | S_CPU | S_ID | S_WEIGHT | S_DATA_SRC | S_WEIGHT_STRUCT
B_HW_INDEX = 1 << 17
PERF_RECORD_MMAP, PERF_RECORD_COMM, PERF_RECORD_SAMPLE, PERF_RECORD_MMAP2 = 1, 3, 9, 10
CTX_FLOOR = 0xFFFFFFFFFFFFF001


def _open(path):
    f = open(path, 'rb')
    return mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)


def attrs(path):
    mm = _open(path)
    attr_size = struct.unpack_from('<Q', mm, 16)[0]
    off, size = struct.unpack_from('<QQ', mm, 24)
    out = []
    for i in range(size // attr_size):
        b = off + i * attr_size
        typ, _sz, config, period, stype, rfmt, flags = struct.unpack_from('<IIQQQQQ', mm, b)
        bstype = struct.unpack_from('<Q', mm, b + 72)[0] if attr_size - 16 >= 80 else 0
        ioff, isz = struct.unpack_from('<QQ', mm, b + attr_size - 16)
        ids = struct.unpack_from(f'<{isz // 8}Q', mm, ioff) if isz else ()
        out.append(dict(type=typ, config=config, period=period, stype=stype, bstype=bstype,
                        freq=bool(flags & (1 << 10)), precise=(flags >> 15) & 3, ids=ids))
    r = subprocess.run(['perf', 'evlist', '-i', path], capture_output=True, text=True)
    names = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    for i, x in enumerate(out):
        x['name'] = names[i] if len(names) == len(out) else f'attr{i}'
    return out


def samples(path, tid=None, t0=None, t1=None, want_chain=True, want_lbr=True, want_addr=False, want_cpu=False):
    """want_addr=True appends (addr, data_src): the PERF_SAMPLE_ADDR data address and PERF_SAMPLE_DATA_SRC word (perf
    record -d samples both; 0 when the attr does not)."""
    at = attrs(path)
    id2a = {}
    for i, x in enumerate(at):
        for j in x['ids']:
            id2a[j] = i
    for x in at:
        if x['stype'] & S_IP and x['stype'] & ~(SUPPORTED | S_ADDR | S_STREAM_ID):
            sys.exit(f"{path}: {x['name']}: unsupported sample_type {x['stype']:#x}")
    sts = {x['stype'] for x in at if x['stype'] & S_IP}
    common_st = max(sts) if sts else 0
    if not (common_st & S_IDENTIFIER) and len(sts) > 1:
        sys.exit(f"{path}: no PERF_SAMPLE_IDENTIFIER and the sampling attrs disagree on layout ({sts}); cannot parse")
    mm = _open(path)
    data_off, data_size = struct.unpack_from('<QQ', mm, 40)
    unp = struct.unpack_from
    a0 = int(t0 * 1e9) if t0 is not None else 0
    a1 = int(t1 * 1e9) if t1 is not None else 1 << 63
    o, end = data_off, data_off + data_size
    while o < end:
        typ, misc, sz = unp('<IHH', mm, o)
        if sz == 0:
            break
        if typ == PERF_RECORD_SAMPLE:
            p = o + 8
            if common_st & S_IDENTIFIER:
                ident = unp('<Q', mm, p)[0]; p += 8
                ai = id2a.get(ident)
                if ai is None:
                    o += sz; continue
                st = at[ai]['stype']
            else:
                st = common_st   # no IDENTIFIER: every sampling attr shares one layout; ID sits after TIME/ADDR
            ip = unp('<Q', mm, p)[0]; p += 8
            _pid, stid = unp('<II', mm, p); p += 8
            t = unp('<Q', mm, p)[0]; p += 8
            addr = 0
            if st & S_ADDR:
                addr = unp('<Q', mm, p)[0]; p += 8
            if st & S_ID:
                if not (common_st & S_IDENTIFIER):
                    ai = id2a.get(unp('<Q', mm, p)[0])
                p += 8
            if not (common_st & S_IDENTIFIER) and ai is None:
                o += sz; continue
            if (tid is not None and stid != tid) or not (a0 <= t <= a1):
                o += sz; continue
            if st & S_STREAM_ID: p += 8
            cpu = -1
            if st & S_CPU:
                cpu = unp('<I', mm, p)[0]; p += 8
            if st & S_PERIOD: p += 8
            chain = ()
            if st & S_CALLCHAIN:
                nr = unp('<Q', mm, p)[0]; p += 8
                if want_chain:
                    chain = unp(f'<{nr}Q', mm, p)
                p += 8 * nr
            lbr = None
            if st & S_BRANCH:
                nr = unp('<Q', mm, p)[0]; p += 8
                if at[ai]['bstype'] & B_HW_INDEX:
                    p += 8
                if want_lbr:
                    lbr = []
                    for k in range(nr):
                        fr, to, fl = unp('<QQQ', mm, p + 24 * k)
                        lbr.append((fr, to, (fl >> 4) & 0xffff, (fl >> 20) & 0xf))
                p += 24 * nr
            if want_addr:
                if st & (S_WEIGHT | S_WEIGHT_STRUCT): p += 8   # REGS_USER/STACK_USER are refused above, so WEIGHT is next
                dsrc = unp('<Q', mm, p)[0] if st & S_DATA_SRC else 0
                yield ai, ip, t, stid, chain, lbr, (addr, dsrc)
            elif want_cpu:   # opt-in 7-tuple: the sample's CPU (-1 when the attr does not record it)
                yield ai, ip, t, stid, chain, lbr, cpu
            else:
                yield ai, ip, t, stid, chain, lbr
        o += sz


def mmaps(path):
    mm = _open(path)
    data_off, data_size = struct.unpack_from('<QQ', mm, 40)
    unp = struct.unpack_from
    out = []
    o, end = data_off, data_off + data_size
    while o < end:
        typ, misc, sz = unp('<IHH', mm, o)
        if sz == 0:
            break
        if typ == PERF_RECORD_MMAP2:
            pid, _ = unp('<II', mm, o + 8)
            addr, length, pgoff = unp('<QQQ', mm, o + 16)
            fo = o + 40 + 24 + 8
            nul = mm.find(b'\0', fo, o + sz)
            out.append((pid, addr, addr + length, pgoff, mm[fo:nul].decode('utf8', 'replace')))
        elif typ == PERF_RECORD_MMAP:
            pid, _ = unp('<II', mm, o + 8)
            addr, length, pgoff = unp('<QQQ', mm, o + 16)
            nul = mm.find(b'\0', o + 40, o + sz)
            out.append((pid, addr, addr + length, pgoff, mm[o + 40:nul].decode('utf8', 'replace')))
        o += sz
    return out


if __name__ == '__main__':
    import collections
    path = sys.argv[1]
    for x in attrs(path):
        print(x['name'], hex(x['stype']), hex(x['bstype']), x['period'], len(x['ids']))
    c = collections.Counter(); nl = 0
    for ai, ip, t, tid, ch, lbr in samples(path):
        c[ai] += 1
        if lbr: nl += len(lbr)
    print(dict(c), 'lbr entries', nl)

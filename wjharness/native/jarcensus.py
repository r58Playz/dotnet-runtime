#!/usr/bin/env python3
"""jarcensus.py -- how Minecraft's bytecode changed across versions, in the terms our runtime pays for.

  jarcensus.py [--prism 1.16.1,1.21.1,26.2] [LABEL=path.jar ...] [--all-classes] [--out FILE]

  --prism  client jars from $PRISM_HOME/libraries/com/mojang/minecraft/<v>/minecraft-<v>-client.jar
  LABEL=jar any other jar (a mod, a library)
Static only: it counts what the bytecode CONTAINS, not what runs. Read it beside mccmp (which runs), never instead.

Every metric is one a cost here scales with:
  * method size against the inlining caps -- HotSpot inlines <= 35 B always (MaxInlineSize) and <= 325 B when hot
    (FreqInlineSize); tier 2 here: the ordinary limit 60, S2's hot-site limit 200, guarded inlining 120 (mini-wasm.c);
  * methods with an exception table -- each activation pays the R353 record, and the inliner refuses them (EH_INLINE=0);
  * invokeinterface / invokevirtual -- the IC and dispatch path; invokestatic / invokespecial are direct;
  * checkcast / instanceof -- the type-check guards mcinsn counts;
  * invokedynamic by bootstrap -- LambdaMetafactory (a delegate per lambda in IKVM), StringConcatFactory (Java 9+ string
    concatenation), ObjectMethods (a record's equals/hashCode/toString), SwitchBootstraps (Java 21 pattern / enum switch);
  * monitorenter and synchronized methods -- monitors.
Per-1000 rates are per 1000 bytecode INSTRUCTIONS (not bytes). The client jar holds the integrated server too.
"""
import collections, io, os, re, struct, sys, zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401

# opcode -> length in bytes (0 = variable: tableswitch, lookupswitch, wide)
OPLEN = [1] * 256
for o in (0x10, 0x12, 0xa9, 0xbc) + tuple(range(0x15, 0x1a)) + tuple(range(0x36, 0x3b)):
    OPLEN[o] = 2
for o in (0x11, 0x13, 0x14, 0x84, 0xbb, 0xbd, 0xc0, 0xc1, 0xc6, 0xc7) + tuple(range(0x99, 0xa9)) + tuple(range(0xb2, 0xb9)):
    OPLEN[o] = 3
OPLEN[0xc5] = 4
for o in (0xb9, 0xba, 0xc8, 0xc9):
    OPLEN[o] = 5
for o in (0xaa, 0xab, 0xc4):
    OPLEN[o] = 0
NAMES = {0xb6: "invokevirtual", 0xb7: "invokespecial", 0xb8: "invokestatic", 0xb9: "invokeinterface",
         0xba: "invokedynamic", 0xc0: "checkcast", 0xc1: "instanceof", 0xbb: "new", 0xbf: "athrow",
         0xb4: "getfield", 0xb5: "putfield", 0xb2: "getstatic", 0xb3: "putstatic", 0xc2: "monitorenter",
         0xaa: "tableswitch", 0xab: "lookupswitch", 0xbc: "newarray", 0xbd: "anewarray", 0xbe: "arraylength"}
for o in range(0x2e, 0x36):
    NAMES[o] = "array load"
for o in range(0x4f, 0x57):
    NAMES[o] = "array store"


class Cls:
    def __init__(self, b):
        self.b, self.o = b, 0
        if self.u4() != 0xCAFEBABE:
            raise ValueError("not a class")
        self.minor, self.major = self.u2(), self.u2()
        n = self.u2(); self.cp = [None] * n; i = 1
        while i < n:
            t = self.u1()
            if t == 1:
                ln = self.u2(); self.cp[i] = (1, self.b[self.o:self.o + ln].decode("utf8", "replace")); self.o += ln
            elif t in (3, 4):
                self.o += 4; self.cp[i] = (t,)
            elif t in (5, 6):
                self.o += 8; self.cp[i] = (t,); i += 1
            elif t in (7, 8, 16, 19, 20):
                self.cp[i] = (t, self.u2())
            elif t in (9, 10, 11, 12, 17, 18):
                self.cp[i] = (t, self.u2(), self.u2())
            elif t == 15:
                self.cp[i] = (t, self.u1(), self.u2())
            else:
                raise ValueError(f"cp tag {t}")
            i += 1
        self.access = self.u2(); self.this = self.cname(self.u2()); self.u2()
        nif = self.u2(); self.o += 2 * nif          # NOT `self.o += 2 * self.u2()`: += reads self.o before u2() advances it
        for _ in range(self.u2()):                  # fields
            self.o += 6; self.attrs()
        self.methods = []
        for _ in range(self.u2()):
            acc, nm, ds = self.u2(), self.utf(self.u2()), self.utf(self.u2())
            self.methods.append((acc, nm, ds, self.attrs()))
        self.cattrs = self.attrs()

    def u1(self): v = self.b[self.o]; self.o += 1; return v
    def u2(self): v = struct.unpack_from(">H", self.b, self.o)[0]; self.o += 2; return v
    def u4(self): v = struct.unpack_from(">I", self.b, self.o)[0]; self.o += 4; return v
    def utf(self, i): return self.cp[i][1]
    def cname(self, i): return self.utf(self.cp[i][1])

    def attrs(self):
        out = {}
        for _ in range(self.u2()):
            nm = self.utf(self.u2()); ln = self.u4()
            out[nm] = self.b[self.o:self.o + ln]; self.o += ln
        return out

    def bsm_owner(self, bsm_index):
        """'Class.method' of a bootstrap method."""
        ba = self.cattrs.get("BootstrapMethods")
        if ba is None:
            return "?"
        o = 2
        for k in range(struct.unpack_from(">H", ba, 0)[0]):
            ref, nargs = struct.unpack_from(">HH", ba, o)
            if k == bsm_index:
                _, _kind, mref = self.cp[ref]
                _, ci, nti = self.cp[mref]
                return self.cname(ci).split("/")[-1] + "." + self.utf(self.cp[nti][1])
            o += 4 + 2 * nargs
        return "?"


INDY = (("LambdaMetafactory", "indy: lambda / method ref"), ("StringConcatFactory", "indy: string concat"),
        ("ObjectMethods", "indy: record equals/hashCode/toString"), ("SwitchBootstraps", "indy: pattern/enum switch"))


def census(jar, all_classes):
    S = collections.Counter(); sizes = []; ops = collections.Counter(); ninsn = 0; majors = collections.Counter()
    eh_bytes = 0; tot_bytes = 0
    with zipfile.ZipFile(jar) as z:
        for n in z.namelist():
            if not n.endswith(".class") or n.startswith("META-INF/"):
                continue
            # The client jar's own code: obfuscated (default package, net/minecraft, com/mojang) or not. Bundled
            # libraries are separate jars on the classpath, so nearly everything in a client jar is Mojang's.
            if not all_classes and "/" in n and not n.startswith(("net/minecraft/", "com/mojang/")):
                continue
            try:
                c = Cls(z.read(n))
            except Exception:
                S["unparsed classes"] += 1
                continue
            S["classes"] += 1; majors[c.major] += 1
            iface = bool(c.access & 0x0200)
            S["interfaces"] += iface
            S["enums"] += bool(c.access & 0x4000)
            S["records"] += "Record" in c.cattrs
            S["sealed (PermittedSubclasses)"] += "PermittedSubclasses" in c.cattrs
            for acc, nm, ds, at in c.methods:
                code = at.get("Code")
                if iface and code is not None and not acc & 0x0008 and not acc & 0x0002 and nm != "<clinit>":
                    S["interface default methods"] += 1
                if code is None:
                    continue
                S["methods with code"] += 1
                S["synchronized methods"] += bool(acc & 0x0020)
                S["lambda bodies (synthetic lambda$)"] += nm.startswith("lambda$")
                ln = struct.unpack_from(">I", code, 4)[0]
                bc = code[8:8 + ln]
                ne = struct.unpack_from(">H", code, 8 + ln)[0]
                sizes.append(ln); tot_bytes += ln
                if ne:
                    S["methods with an exception table"] += 1; eh_bytes += ln
                    for k in range(ne):
                        ct = struct.unpack_from(">H", code, 10 + ln + 8 * k + 6)[0]
                        S["finally handlers (catch any)" if ct == 0 else "catch handlers (typed)"] += 1
                pc = 0
                while pc < ln:
                    op = bc[pc]; ninsn += 1
                    nm_ = NAMES.get(op)
                    if nm_:
                        ops[nm_] += 1
                    if op == 0xba:
                        idx = struct.unpack_from(">H", bc, pc + 1)[0]
                        own = c.bsm_owner(c.cp[idx][1])
                        ops[next((lab for k, lab in INDY if own.startswith(k)), "indy: other (" + own + ")")] += 1
                    L = OPLEN[op]
                    if L:
                        pc += L
                    elif op == 0xc4:
                        pc += 6 if bc[pc + 1] == 0x84 else 4
                    else:
                        base = (pc + 4) & ~3
                        if op == 0xaa:
                            lo, hi = struct.unpack_from(">ii", bc, base + 4)
                            pc = base + 12 + 4 * (hi - lo + 1)
                        else:
                            npairs = struct.unpack_from(">i", bc, base + 4)[0]
                            pc = base + 8 + 8 * npairs
    sizes.sort()
    return dict(S=S, sizes=sizes, ops=ops, ninsn=ninsn, majors=majors, eh_bytes=eh_bytes, tot_bytes=tot_bytes)


def main():
    a = sys.argv[1:]
    jars = []
    if "--prism" in a:
        ph = os.environ.get("PRISM_HOME") or sys.exit("PRISM_HOME is not configured (wj paths)")
        for v in a[a.index("--prism") + 1].split(","):
            jars.append((v, os.path.join(ph, "libraries", "com", "mojang", "minecraft", v, f"minecraft-{v}-client.jar")))
    jars += [tuple(x.split("=", 1)) for x in a if "=" in x and not x.startswith("--")]
    if not jars:
        sys.exit(__doc__.split("\n\n")[1])
    R = [(lab, census(j, "--all-classes" in a)) for lab, j in jars]
    out = []
    def row(name, f, fmt="{:,.0f}"):
        out.append(f"| {name} | " + " | ".join(fmt.format(f(r)) if f(r) is not None else "" for _, r in R) + " |")
    out.append("| | " + " | ".join(lab for lab, _ in R) + " |")
    out.append("|---|" + "---|" * len(R))
    row("class file major (most classes)", lambda r: r["majors"].most_common(1)[0][0], "{}")
    for k in ("classes", "interfaces", "enums", "records", "sealed (PermittedSubclasses)", "interface default methods",
              "methods with code"):
        row(k, lambda r, k=k: r["S"][k])
    row("bytecode, KB", lambda r: r["tot_bytes"] / 1024)
    row("bytecode instructions, K", lambda r: r["ninsn"] / 1000)
    pct = lambda xs, q: xs[min(len(xs) - 1, int(q * len(xs)))]
    row("method size, median B", lambda r: pct(r["sizes"], 0.5))
    row("method size, p90 B", lambda r: pct(r["sizes"], 0.9))
    for lo, hi, why in ((0, 35, "<= 35 B (HotSpot always inlines)"), (36, 60, "36-60 B (our tier-2 ordinary limit)"),
                        (61, 120, "61-120 B (our guarded-inline cap)"), (121, 325, "121-325 B (HotSpot inlines when hot)"),
                        (326, 1 << 30, "> 325 B")):
        row(f"methods {why}, %", lambda r, lo=lo, hi=hi: 100 * sum(1 for s in r["sizes"] if lo <= s <= hi) / len(r["sizes"]), "{:.1f}")
    row("methods with an exception table, %", lambda r: 100 * r["S"]["methods with an exception table"] / r["S"]["methods with code"], "{:.2f}")
    row("bytecode in methods with an exception table, %", lambda r: 100 * r["eh_bytes"] / r["tot_bytes"], "{:.1f}")
    row("catch handlers (typed)", lambda r: r["S"]["catch handlers (typed)"])
    row("finally handlers (catch any)", lambda r: r["S"]["finally handlers (catch any)"])
    row("synchronized methods", lambda r: r["S"]["synchronized methods"])
    # (no lambda-body count: before 26.x the jar is obfuscated and ProGuard renames lambda$ bodies; count the indy sites)
    out.append("| **per 1000 bytecode instructions** | " + " | " * len(R))
    keys = ["invokevirtual", "invokeinterface", "invokestatic", "invokespecial", "invokedynamic"] + [lab for _, lab in INDY]
    keys += ["checkcast", "instanceof", "new", "getfield", "putfield", "getstatic", "array load", "array store",
             "tableswitch", "lookupswitch", "monitorenter", "athrow"]
    for k in keys:
        row(k, lambda r, k=k: 1000 * r["ops"][k] / r["ninsn"], "{:.2f}")
    row("invokeinterface share of calls, %", lambda r: 100 * r["ops"]["invokeinterface"] /
        sum(r["ops"][k] for k in ("invokevirtual", "invokeinterface", "invokestatic", "invokespecial", "invokedynamic")), "{:.1f}")
    other = collections.Counter()
    for _, r in R:
        for k, v in r["ops"].items():
            if k.startswith("indy: other"):
                other[k] += v
    txt = "\n".join(out)
    if other:
        txt += "\n\nOther invokedynamic bootstraps (all versions): " + ", ".join(f"{k[13:-1]} {v}" for k, v in other.most_common(8))
    if "--out" in a:
        open(a[a.index("--out") + 1], "w").write(txt + "\n")
    print(txt)


if __name__ == "__main__":
    main()

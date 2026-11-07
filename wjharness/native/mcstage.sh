#!/usr/bin/env bash
# mcstage.sh -- build a STANDALONE native Minecraft stage that mcnative.sh launches, under $WJ_STATE/native/<name>/.
#
# WHY. natrun.sh plays the Prism instance in place: it swaps that instance's save, reads its options.txt and loads its
# mods/, so a manual session changes the workload (renderDistance 32, 2026-10-05) and the instance's mod set is not
# ours (it holds mcsrranked 6.0.4 beside 5.8.1; ours ships 5.8.1 alone). A stage takes every WORKLOAD file from the
# tar ours plays (mods, config, options.txt, the save, the client jar), and from Prism ($PRISM_HOME) only what ours has
# no native counterpart of -- LWJGL and its linux natives, and the asset objects -- plus the maven libraries, all
# read-only and sha1-checked against the version json. A library Prism lacks is taken from the tar's own
# ikvmcraft/libraries (the copy ours runs), sha1-checked the same way. Nothing under Prism is written.
#
#   mcstage.sh [--tar T] [--version V] [--name N]
#     defaults: --tar $NW_TAR --version 1.16.1-fabric-0.19.3 --name sa   (1.16.1 + MCSR Ranked, the R458 workload)
#     26.2 no-mods: --tar $WJ_STATE/seeds/ikvmcraft-262n.tar --version 26.2-fabric-0.19.5 --name sa262n
#
# Output: cp.txt (the classpath), natives/ (LWJGL .so), template/ (the tar's minecraft/ game dir, minus Fabric's .fabric
# cache), client.jar, lib/ (libraries Prism lacked), launch.env (main class, asset index, version, join method),
# STAGE.txt (provenance). The old stage is replaced only when the new one is complete.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
TAR=""; VER="1.16.1-fabric-0.19.3"; NAME="sa"
while [ $# -gt 0 ]; do
  case "$1" in
    --tar) TAR=$2; shift ;;
    --version) VER=$2; shift ;;
    --name) NAME=$2; shift ;;
    *) echo "mcstage: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$TAR" ] || TAR=$(need NW_TAR "the tar ours plays: the stage's mods, config, options and save come from it")
PH=$(need PRISM_HOME "Prism Launcher's data dir (libraries/, assets/, meta/)")
SA="$WJ_STATE/native/$NAME"
NEW="$SA.new"
rm -rf "$NEW"; mkdir -p "$NEW/template" "$NEW/natives" "$NEW/v" "$NEW/lib"
echo "[stage] $NAME: $VER from $TAR"
tar -xf "$TAR" -C "$NEW/template" --strip-components=1 minecraft/
# Fabric's remap cache was written by the browser run; let the native loader build its own.
rm -rf "$NEW/template/.fabric"
tar -xf "$TAR" -C "$NEW/v" --strip-components=3 "ikvmcraft/versions/$VER/version.json"
RD=$(grep -m1 '^renderDistance:' "$NEW/template/options.txt" | cut -d: -f2 | tr -d '\r')
[ "$RD" = 12 ] || { echo "[stage] REFUSING: the tar's options.txt has renderDistance=$RD, ours plays 12"; exit 1; }
[ -d "$NEW/template/saves/New World" ] || { echo "[stage] REFUSING: no 'New World' save in the tar"; exit 1; }

python3 - "$NEW" "$PH" "$SA" "$TAR" "$VER" <<'EOF'
import hashlib, json, os, sys, tarfile, zipfile
new, ph, final, tarpath, ver = sys.argv[1:6]   # cp.txt names the FINAL paths (new is renamed)
lib = os.path.join(ph, "libraries")
def sha1(p):
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()
def allowed(l):
    """Mojang/Prism rules for linux x86-64. Prism spells other architectures as os names (linux-arm64, ...)."""
    rules = l.get("rules")
    if not rules: return True
    ok = False
    for r in rules:
        os_ = r.get("os") or {}
        if os_.get("name") in (None, "linux") and os_.get("arch") in (None, "x86_64", "x86-64", "amd64"):
            ok = r["action"] == "allow"
    return ok
def mpath(name):
    p = name.split(":"); g, a, v = p[:3]; c = p[3] if len(p) > 3 else None
    return os.path.join(*g.split("."), a, v, f"{a}-{v}" + (f"-{c}" if c else "") + ".jar")
tf = tarfile.open(tarpath)
tarnames = {m.name.lstrip("./"): m for m in tf.getmembers() if m.isfile()}
cp, missing, bad, from_tar = [], [], [], 0
def resolve(rel, want):
    """A library by its maven-relative path: Prism's copy, else the tar's, sha1-checked when the json gives one."""
    global from_tar
    p = os.path.join(lib, rel)
    if os.path.exists(p) and (not want or sha1(p) == want):
        return p
    m = tarnames.get("ikvmcraft/libraries/" + rel)
    if m is not None:
        q = os.path.join(new, "lib", rel); os.makedirs(os.path.dirname(q), exist_ok=True)
        open(q, "wb").write(tf.extractfile(m).read())
        if not want or sha1(q) == want:
            from_tar += 1
            return os.path.join(final, "lib", rel)
        bad.append(q); return None
    (bad if os.path.exists(p) else missing).append(p)
    return None
vj = json.load(open(os.path.join(new, "v", "version.json")))
nat = os.path.join(new, "natives")
def natives_from(path):
    with zipfile.ZipFile(new + path[len(final):] if path.startswith(final) else path) as z:
        for n in z.namelist():
            if n.endswith(".so") and "/arm" not in n and "riscv" not in n:
                open(os.path.join(nat, os.path.basename(n)), "wb").write(z.read(n))
seen = set()
for l in vj["libraries"]:
    if "natives" in l or not allowed(l):
        continue                      # natives-only entries, macOS / Windows libraries
    a = (l.get("downloads") or {}).get("artifact") or {}
    p = resolve(a.get("path") or mpath(l["name"]), a.get("sha1"))
    if p and l["name"] not in seen:
        seen.add(l["name"]); cp.append(p)
req = [r for r in vj.get("requires", []) if r.get("uid") == "org.lwjgl3"]
lwv = (req[0].get("equals") or req[0].get("suggests")) if req else None
if lwv:
    lw = json.load(open(os.path.join(ph, "meta", "org.lwjgl3", f"{lwv}.json")))
    for l in lw["libraries"]:
        if l["name"].split(":")[2] != lwv or not allowed(l):
            continue                  # the -lwjgl.1 / gman / mmachina arms are other architectures
        d = l.get("downloads") or {}
        if "natives" in l:            # LWJGL 3.2.x: a classifier on the module
            c = (d.get("classifiers") or {}).get("natives-linux")
            if c:
                p = resolve(c.get("path") or mpath(l["name"] + ":natives-linux"), c.get("sha1"))
                if p: natives_from(p)
        else:                          # LWJGL 3.3+: natives are separate artifacts (lwjgl-glfw-natives-linux), on the cp too
            a = d.get("artifact") or {}
            p = resolve(a.get("path") or mpath(l["name"]), a.get("sha1"))
            if p:
                cp.append(p)
                if "natives-linux" in l["name"]:
                    natives_from(p)
parent = vj.get("inheritsFrom")
cj = f"ikvmcraft/versions/{parent or ver}/client.jar"
if cj not in tarnames:
    sys.exit(f"[stage] REFUSING: no {cj} in the tar")
open(os.path.join(new, "client.jar"), "wb").write(tf.extractfile(tarnames[cj]).read())
want = vj["mainJar"]["downloads"]["artifact"]["sha1"]
if sha1(os.path.join(new, "client.jar")) != want:
    bad.append("client.jar")
if missing or bad:
    sys.exit("[stage] REFUSING:\n" + "".join(f"  missing {p}\n" for p in missing) + "".join(f"  sha1 mismatch {p}\n" for p in bad))
cp.append(os.path.join(final, "client.jar"))
open(os.path.join(new, "cp.txt"), "w").write(":".join(cp) + "\n")
md = os.path.join(new, "template", "mods")
mods = sorted(os.listdir(md)) if os.path.isdir(md) else []
# How mcnative gets into the world: the MCSR Ranked set needs natrun's menu replay (Ranked intercepts --quickPlay* and
# shows an auth dialog first); anything else joins with --quickPlaySingleplayer (1.20+), which needs no keyboard.
join = "mcsr" if any(m_.startswith("mcsrranked") for m_ in mods) else "quickplay"
with open(os.path.join(new, "launch.env"), "w") as f:
    f.write(f"MC_MAIN={vj['mainClass']}\nMC_ASSET_INDEX={vj['assetIndex']['id']}\nMC_GAME_VERSION={parent or ver}\n"
            f"MC_VERSION_TYPE={vj.get('type', 'release')}\nMC_JOIN={join}\n")
with open(os.path.join(new, "STAGE.txt"), "w") as f:
    f.write(f"version {ver}\nmain {vj['mainClass']}\nassetIndex {vj['assetIndex']['id']}\nlwjgl {lwv}\nclient.jar sha1 {want}\n"
            f"join {join}\nlibraries from the tar (absent from Prism) {from_tar}\n")
    for p in cp[:-1]: f.write(f"cp {p}\n")
    for m_ in mods:
        f.write(f"mod {m_} sha256 {hashlib.sha256(open(os.path.join(md, m_), 'rb').read()).hexdigest()[:16]}\n")
print(f"[stage] classpath {len(cp)} jars ({from_tar} from the tar), natives {len(os.listdir(nat))} .so, {len(mods)} mods, join {join}")
EOF
[ -e "$NEW/natives/liblwjgl.so" ] || { echo "[stage] REFUSING: no liblwjgl.so extracted"; exit 1; }
{ echo "tar $TAR $(stat -c '%s %y' "$TAR")"; echo "prism $PH"; echo "built $(date -Is)"; } >> "$NEW/STAGE.txt"
rm -rf "$NEW/v" "$SA.old"
[ -e "$SA" ] && mv "$SA" "$SA.old"
mv "$NEW" "$SA"; rm -rf "$SA.old"
echo "[stage] ok: $SA"; grep -E '^mod ' "$SA/STAGE.txt" | sed 's/^/[stage]   /' || true

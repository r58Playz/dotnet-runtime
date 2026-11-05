"""The harness's locations for Python tools, and the import path.

Importing this module (a) puts every harness code directory on sys.path, so `import perfraw`, `import bootattr`,
`from counts import run_counts` work from any directory, and (b) makes the resolved configuration available as
`paths.cfg(KEY)` / `paths.need(KEY)`. Resolution itself lives in lib/paths.mjs (one implementation): under `wj`
the values are already in the environment; run directly, this asks node once.

Use from a tool in any subdirectory:

    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa
"""
import json
import os
import subprocess
import sys

HARNESS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(HARNESS)
CODE_DIRS = ["lib", "drive", "build", "read", "fe", "ledger", "tier", "native", "boot", "memaudit", "j2d", "live", "enctest"]

for _d in reversed(CODE_DIRS):
    _p = os.path.join(HARNESS, _d)
    if _p not in sys.path:
        sys.path.insert(1, _p)

_resolved = None


def _load():
    global _resolved
    if _resolved is None:
        if os.environ.get("WJ_HARNESS") == HARNESS and os.environ.get("WJ_HOME"):
            _resolved = dict(os.environ)
        else:
            out = subprocess.run(["node", os.path.join(HARNESS, "lib", "paths.mjs"), "--json"],
                                 capture_output=True, text=True, check=True).stdout
            _resolved = json.loads(out)
            for k, v in _resolved.items():
                os.environ.setdefault(k, v)
    return _resolved


def cfg(key, default=None):
    v = _load().get(key)
    return v if v not in (None, "") else default


def need(key, why=""):
    v = cfg(key)
    if v is None:
        f = cfg("WJ_MACHINE_ENV", "~/.config/wjharness/machine.env")
        sys.exit(f"{key} is not configured{' (' + why + ')' if why else ''}: set it in {f} "
                 f"(see {os.path.join(HARNESS, 'machine.env.example')}) or the environment")
    return v


def wj_home():
    return os.path.abspath(need("WJ_HOME"))


def state(*p):
    return os.path.join(wj_home(), "state", *p)


def runs(*p):
    return os.path.join(wj_home(), "runs", *p)


def tool(name):
    """Path of a harness tool by file name, e.g. tool("servertid.py")."""
    for d in CODE_DIRS:
        p = os.path.join(HARNESS, d, name)
        if os.path.exists(p):
            return p
    raise FileNotFoundError(f"no harness tool named {name}")


def cache_dir(*p):
    """Per-user cache for derived data (symbol pickles etc.); never inside the repo."""
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    return os.path.join(base, *p)

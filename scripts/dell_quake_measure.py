"""Matched yquake2 GL3 1280x720 600-record demo on the Dell, with GPU diagnostics.

Per arm: swap trace, per-stage submission timing for one watched owner, gpu-submit worker
occupancy (`gpu worker stats`, reset at launch and read at exit) and desktop renderer
displayed-frame counters (/tmp/brook-desktop-stats.json before/after; compare with
scripts/desktop_stats_diff.py).

Usage (repo root, bridge registry in BROOK_ROOT):
  python3 scripts/dell_quake_measure.py OUTPUT_DIR SESSION PAYLOAD [ARMS]
ARMS is a comma list of A (watch Quake, 8 buffers), B (watch renderer, 8), C (watch Quake, 2),
D (watch Quake, 4)."""
import json, re, sys, tarfile, textwrap, time, uuid
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/vm_bridge")]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=False)
session, payload = sys.argv[2:4]
arms = sys.argv[4].split(",") if len(sys.argv) > 4 else ["A", "B", "C"]
h = Hardware()
SHARED = Path("/data/Programming/Brook/shared/dell")
key = SHARED / "hardware-update-key"
py, bash = "/nix/profile/bin/python3", "/nix/profile/bin/bash"
quake = "/nix/store/.brook-gpu-stage-c6f05db4/sb7y1c75d6kkflpcib9g8mlipjjy1xrz-yquake2-brook-8.60/lib/yquake2/quake2"
iris_run = "/nix/store/998k7n885vyvxlsckcc5nf9avj3d540n-mesa-26.0.6/bin/brook-iris-run"
work = "/tmp/brook-review-" + uuid.uuid4().hex[:8]

def save(folder, name, value):
    (folder / (name + ".json")).write_text(json.dumps(value, indent=2) + "\n"); return value

def ident(folder, name):
    v = uuid.uuid4().hex; (folder / (name + "-id")).write_text(v + "\n"); return v

def run(folder, name, argv, uid=1000, env=None, tasks=10):
    return save(folder, name, h.guest_run(session, ident(folder, name), argv, cwd="/tmp", uid=uid,
                                          env=env, wait_s=2, task_limit=tasks))

def finish(folder, name, r, timeout=60):
    end = time.monotonic() + timeout
    while r["state"] not in ("completed", "failed") and time.monotonic() < end:
        r = h.job(session, r["operation_id"], wait_s=4, output_limit=16384)
    save(folder, name + "-final", r)
    if r["state"] != "completed" or r["guest"]["live_tasks"]:
        raise RuntimeError("recover job " + r["operation_id"])
    return r

def fetch(folder, name, path):
    r = finish(folder, name, run(folder, name, ["/nix/profile/bin/cat", path]))
    s = r["stdout"]; parts = [s["text"]]
    while not s["eof"]:
        s = h.job(session, r["operation_id"], stdout_offset=s["next_offset"], output_limit=16384)["stdout"]
        parts.append(s["text"])
    return "".join(parts)

def cmd(c): return h.command(session, c)

d = save(out, "doctor", h.doctor(session))
assert d["identity_matches"] and d["guest"]["payload_id"] == payload
bundle = BulkRun(h, out / "bundle-transfer").execute(
    session, key, "put", source=SHARED / "quake-probe-bundle.tar",
    request_id=ident(out, "bundle-transfer"))
setup = r"""
import os, sys, tarfile
from pathlib import Path
source, work = Path(sys.argv[1]), Path(sys.argv[2])
base = work / "quake2/baseq2"; base.mkdir(parents=True)
files = {"q2_demo_timing.py": work, "swap.so": work, "pak1.pak": base, "pak2.pak": base}
with tarfile.open(source, "r:") as tar:
    for name, target in files.items():
        with tar.extractfile(name) as f: (target / name).write_bytes(f.read())
os.symlink("/data/home/quake2/baseq2/pak0.pak", base / "pak0.pak")
for root, dirs, names in os.walk(work):
    for n in dirs + names: os.chown(os.path.join(root, n), 1000, 1000)
os.chown(work, 1000, 1000)
"""
r = finish(out, "setup", run(out, "setup", [py, "-c", textwrap.dedent(setup), bundle["transfer"]["guest_path"], work], uid=0))
assert r["exit"]["code"] == 0, r["stderr"]["text"]

def renderer_owner():
    procs = cmd("procs")["reply"]
    m = [int(x) for x in re.findall(r"pid=(\d+) state=\d+ cpu=-?\d+ 'VKRENDER\.ELF'", procs)]
    assert len(m) == 1, procs
    return m[0]

def arm(label, watch, buffers):
    f = out / label; f.mkdir()
    home = f"{work}/{label}-demo"
    env = {"XDG_RUNTIME_DIR": "/tmp", "WAYLAND_DISPLAY": "wayland-0", "SDL_AUDIODRIVER": "dummy",
           "LD_PRELOAD": work + "/swap.so", "BROOK_SWAP_TRACE": home + "-swaps.csv",
           "BROOK_SWAP_SETTINGS": home + "-swap-settings.json", "BROOK_MESA_WAYLAND_BUFFERS": str(buffers)}
    save(f, "bcs-before", cmd("bridge hardware gpu-bcs-present"))
    save(f, "desktop-stats-before", {"text": fetch(f, "read-stats-before", "/tmp/brook-desktop-stats.json")})
    owner = None
    if watch == "renderer":
        owner = renderer_owner()
        save(f, "watch", cmd(f"gpu submission timing watch {owner}"))
    t0 = time.monotonic()
    save(f, "worker-reset", cmd("gpu worker stats reset"))
    game = run(f, "game", ["/nix/profile/bin/brook-run", iris_run, py, work + "/q2_demo_timing.py", "--engine", quake,
                           "--home", home, "--pak-dir", work + "/quake2/baseq2", "--uncapped",
                           "--measured-frames", "600"], env=env, tasks=60)
    if watch == "game":
        end = time.monotonic() + 60
        while time.monotonic() < end and owner is None:
            game = h.job(session, game["operation_id"], wait_s=1, output_limit=16384)
            pid = re.search(r"Q2_ENGINE_STARTED (\d+)", game.get("stdout",{}).get("text",""))
            if pid:
                ws = [w for w in h.windows(session)["windows"] if w.get("client", {}).get("pid") == int(pid[1])]
                if len(ws) == 1:
                    owner = ws[0]["client"]["incarnation"]; save(f, "game-window", ws[0])
                    save(f, "watch", cmd(f"gpu submission timing watch {owner}"))
            if game["state"] in ("completed", "failed"): break
    game = finish(f, "game", game, timeout=180)
    save(f, "worker-stats", cmd("gpu worker stats"))
    save(f, "desktop-stats-after", {"text": fetch(f, "read-stats-after", "/tmp/brook-desktop-stats.json")})
    save(f, "elapsed", {"host_s": time.monotonic() - t0, "owner": owner, "watch": watch, "buffers": buffers})
    save(f, "submission-timing", cmd("gpu submission timing status"))
    save(f, "submission-timing-off", cmd("gpu submission timing off"))
    save(f, "bcs-after", cmd("bridge hardware gpu-bcs-present"))
    save(f, "stats", h.stats(session))
    print(label, "exit", game["exit"], game["stdout"]["text"][-600:], flush=True)
    for name, path in (("swaps.csv", home + "-swaps.csv"), ("quake.log", home + "/timing-stdout.log"),
                       ("timing.json", home + "/timing-result.json")):
        try: (f / name).write_text(fetch(f, "read-" + name, path))
        except Exception as e: print("fetch failed", name, e)

plan = {"A": ("game", 8), "B": ("renderer", 8), "C": ("game", 2), "D": ("game", 4)}
for a in arms:
    arm(a, *plan[a])
print("done", out)

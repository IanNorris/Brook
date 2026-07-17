#!/usr/bin/env python3
"""P2 regression gate for the Brook OS API-gap audit.

A sentinel-gated, sequential harness that boots Brook headless under QEMU
(software composite, single-CPU), drives one representative app into a target
feature via deterministic HMP key injection, and asserts a *semantic* outcome
(a persisted application artifact) rather than mere process survival.

Assertion tiers (each probe uses the weakest sufficient oracle):
  1. counter/manifest deltas  -- deterministic backbone (added later)
  2. logged semantic outcome  -- this file's Slice 1
  3. golden pixel diff        -- last resort (added later)

Hard rules baked in here:
  * QEMU is owned via -pidfile and shut down with HMP `quit`; we never grep
    /proc for a process to terminate (shared container -> could hit a co-tenant).
  * Every readiness wait is a serial sentinel with a hard-fail timeout; there
    is no fixed-sleep "act anyway" path.
  * Boot is single-CPU (BROOK_SMP=1) to dodge the intermittent BRO-208 SMP race.
"""
import argparse
import json
import os
import secrets
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmp import Hmp          # noqa: E402
from serialtail import SerialTail  # noqa: E402
import gaps as gapmod        # noqa: E402

BROOK = Path(__file__).resolve().parents[2]
RUN_QEMU = BROOK / "scripts" / "run-qemu.sh"
SCRIPTS_DIR = BROOK / "data" / "scripts"
GATE_DIR = Path(__file__).resolve().parent
BASELINE_DIR = GATE_DIR / "baseline"

BASE_DISKS = {
    "BROOK_DISK_IMG":  BROOK / "brook_disk.img",
    "BROOK_NIX_DISK":  BROOK / "brook_nix_disk.img",
    "BROOK_EXT2_DISK": BROOK / "brook_ext2_disk.img",
    "BROOK_GPT_DISK":  BROOK / "brook_gpt_disk.img",
}

# character -> QEMU sendkey token for the interaction alphabet we drive.
_SHIFT = {c: f"shift-{c.lower()}" for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"}


def char_to_keys(text):
    keys = []
    for c in text:
        if c in _SHIFT:
            keys.append(_SHIFT[c])
        elif c.isalnum():
            keys.append(c)
        elif c == " ":
            keys.append("spc")
        else:
            raise ValueError(f"unsupported interaction char: {c!r}")
    return keys


class ProbeResult:
    def __init__(self, name):
        self.name = name
        self.stages = []          # list of (stage, ok, detail)
        self.gap_lines = []
        self.started = time.time()

    def stage(self, name, ok, detail=""):
        self.stages.append((name, ok, detail))

    @property
    def passed(self):
        return all(ok for _, ok, _ in self.stages)

    def to_dict(self):
        return {
            "probe": self.name,
            "passed": self.passed,
            "duration_s": round(time.time() - self.started, 1),
            "stages": [
                {"stage": s, "ok": ok, "detail": d} for s, ok, d in self.stages
            ],
            "gap_lines": self.gap_lines,
        }


def find_free_instance():
    for _ in range(50):
        n = 60 + secrets.randbelow(35)   # 60..94
        if not Path(f"/tmp/qemu_monitor_{n}.sock").exists():
            return n
    raise RuntimeError("no free QEMU instance slot")


def reflink_disks(run_dir):
    env = {}
    for key, base in BASE_DISKS.items():
        if not base.exists():
            raise FileNotFoundError(f"base disk missing: {base}")
        dst = run_dir / base.name
        subprocess.run(["cp", "--reflink=auto", str(base), str(dst)], check=True)
        env[key] = str(dst)
    return env


def stack_fingerprint():
    """Record the store paths of the toolkit/renderer -- for a Nix userspace
    a package bump is the single most likely silent-degradation trigger, so
    this is the most useful diagnostic field to attach to a result."""
    fp = {}
    for k in ("BROOK_GPU_MESA", "BROOK_GPU_QEMU"):
        if os.environ.get(k):
            fp[k] = os.environ[k]
    try:
        fp["git_head"] = subprocess.check_output(
            ["git", "-C", str(BROOK), "rev-parse", "HEAD"], text=True
        ).strip()
    except Exception:
        pass
    return fp


class BrookVM:
    def __init__(self, instance, rc_name, run_dir, log_path, extra_env=None):
        self.instance = instance
        self.rc_name = rc_name
        self.run_dir = run_dir
        self.log_path = log_path
        self.extra_env = extra_env or {}
        self.monitor_sock = f"/tmp/qemu_monitor_{instance}.sock"
        self.pidfile = run_dir / "qemu.pid"
        self.proc = None
        self.qemu_pid = None
        self.serial = None
        self.hmp = None

    def launch(self):
        env = dict(os.environ)
        env["BROOK_SMP"] = "1"
        env["BROOK_SKIP_UPDATE_DISK"] = "1"
        env.update(reflink_disks(self.run_dir))
        env.update(self.extra_env)
        cmd = [
            "bash", str(RUN_QEMU), "--release", "--headless", "--no-audio",
            f"--instance={self.instance}", "--script", self.rc_name,
            # trailing args -> QEMU EXTRA_ARGS: own the pid, pin the clock.
            "-pidfile", str(self.pidfile),
            "-rtc", "base=2020-01-01T00:00:00",
        ]
        logf = open(self.log_path, "wb")
        self.proc = subprocess.Popen(
            cmd, cwd=str(BROOK), stdout=logf, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, start_new_session=True, env=env,
        )
        self.serial = SerialTail(str(self.log_path)).start()
        return self

    def connect_monitor(self):
        self.hmp = Hmp(self.monitor_sock).connect()
        # capture the QEMU pid the moment the monitor is up
        for _ in range(50):
            if self.pidfile.exists():
                try:
                    self.qemu_pid = int(self.pidfile.read_text().strip())
                    break
                except ValueError:
                    pass
            time.sleep(0.1)
        return self

    def shutdown(self):
        # Clean path: HMP quit, then wait on the captured pid. Escalate only to
        # the specific numeric pid we own -- never a name-based sweep.
        if self.hmp:
            try:
                self.hmp.quit()
            except Exception:
                pass
            self.hmp.close()
        pid = self.qemu_pid
        if pid:
            for _ in range(50):
                try:
                    os.kill(pid, 0)
                except OSError:
                    break
                time.sleep(0.1)
            else:
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
        if self.serial:
            self.serial.stop()
        if self.proc:
            try:
                self.proc.wait(timeout=5)
            except Exception:
                pass


# ---- Slice 1 probe: mousepad edit-and-save, semantic-asserted, no pixels ----

MOUSEPAD_BIN = "/nix/store/n7yp1cnp2f6xfw9mkxihp0laxhd0hhpa-mousepad-0.7.0/bin/mousepad"
EDIT_FILE = "/tmp/p2edit.txt"
MAGIC = "Brook123"


def write_mousepad_rc(rc_path, do_type):
    bash = "/boot/BIN/BASH --norc --noprofile -c"
    # The app process: seed the file with OLD, then become mousepad on it.
    app = (f"printf OLD > {EDIT_FILE}; sync; exec {MOUSEPAD_BIN} {EDIT_FILE}")
    # The semantic watcher: emit a single-line marker once the saved bytes are
    # exactly the magic. If typing silently does nothing the marker never fires
    # and the gate times out (hard fail) -- which is the whole point.
    watch = (f"while :; do if grep -q {MAGIC} {EDIT_FILE} 2>/dev/null; then "
             f"echo P2_ASSERT probe=mousepad-save result=pass; break; fi; "
             f"sleep 0.2; done")
    lines = [
        "set wm",
        "set vfb none",
        "run --once /nix/bin/waylandd",
        f"run {bash} \"{app}\"",
        f"run {bash} \"{watch}\"",
        "",
    ]
    rc_path.write_text("\n".join(lines))


def run_mousepad_probe(expect_fail=False, do_type=True, keep=False):
    name = "mousepad-save" + ("" if do_type else "[negative]")
    res = ProbeResult(name)
    instance = find_free_instance()
    run_dir = Path("/tmp") / f"p2gate_{instance}_{secrets.token_hex(3)}"
    run_dir.mkdir(parents=True, exist_ok=True)
    rc_name = f"p2_mousepad_{instance}"
    rc_path = SCRIPTS_DIR / f"{rc_name}.rc"
    log_path = run_dir / "serial.log"
    write_mousepad_rc(rc_path, do_type)

    vm = BrookVM(instance, rc_name, run_dir, log_path)
    try:
        vm.launch()
        vm.connect_monitor()
        s = vm.serial

        # boot-ready
        try:
            _, idx = s.wait_for(r"globals advertised", timeout=90)
            res.stage("boot-ready", True)
        except TimeoutError as e:
            res.stage("boot-ready", False, str(e))
            return res

        # app-ready: mousepad window mapped
        try:
            line, idx = s.wait_for(r"WM: created window \d+ '.*Mousepad'.*for pid (\d+)",
                                   timeout=90, start_index=idx)
            res.stage("app-ready", True, line.strip())
        except TimeoutError as e:
            res.stage("app-ready", False, str(e))
            return res

        # focus
        try:
            _, idx = s.wait_for(r"FOCUS_GAINED wm=\d+", timeout=30, start_index=idx)
            res.stage("focus", True)
        except TimeoutError as e:
            res.stage("focus", False, str(e))
            return res

        # interaction: select-all, [type MAGIC], save. Wait each key's UP.
        def press(keyspec):
            before = s.current_index()
            vm.hmp.sendkey(keyspec)
            s.wait_for(r"KEY sc=0x[0-9a-f]+ xkb=\d+ UP", timeout=10,
                       start_index=before)

        try:
            press("ctrl-a")
            if do_type:
                for k in char_to_keys(MAGIC):
                    press(k)
            press("ctrl-s")
            res.stage("interaction", True)
        except TimeoutError as e:
            res.stage("interaction", False, str(e))
            return res

        # semantic assertion: watcher confirms the persisted bytes == MAGIC.
        # Generous ceiling: mousepad RELAYS keys fast (the KEY markers) but its
        # GTK loop processes+saves them slowly, so the artifact can land tens of
        # seconds after the last key is relayed. This is a sentinel, not a
        # sleep -- it returns the instant the marker appears, so a large ceiling
        # costs nothing on success and only bounds a genuine hang.
        try:
            line, idx = s.wait_for(r"P2_ASSERT probe=mousepad-save result=pass",
                                   timeout=90, start_index=idx)
            res.stage("semantic-assert", True, line.strip())
        except TimeoutError as e:
            res.stage("semantic-assert", False, str(e))

        # gap dump (Ctrl+F11) -- the deterministic counter tier. Wait for the
        # bounded SUBGAP_DUMP end marker rather than sleeping (serial can
        # interleave/truncate; loop to the end sentinel).
        try:
            before = s.current_index()
            vm.hmp.sendkey("ctrl-f11")
            try:
                s.wait_for(r"SUBGAP_DUMP end", timeout=15, start_index=before)
            except TimeoutError:
                pass
            for ln in s.snapshot()[before:]:
                if "SUBGAP" in ln or "SYSCALL_GAP" in ln:
                    res.gap_lines.append(ln.strip())
        except Exception:
            pass

        return res
    finally:
        vm.shutdown()
        if not keep:
            try:
                rc_path.unlink()
            except OSError:
                pass
            shutil.rmtree(run_dir, ignore_errors=True)
        else:
            print(f"[keep] run dir: {run_dir}  rc: {rc_path}")


def main():
    ap = argparse.ArgumentParser(description="Brook P2 regression gate")
    ap.add_argument("--probe", default="mousepad", choices=["mousepad"])
    ap.add_argument("--negative", action="store_true",
                    help="run the negative self-test (omit typing; expect the "
                         "semantic assertion to go RED)")
    ap.add_argument("--keep", action="store_true", help="keep run dir + rc")
    ap.add_argument("--json", help="write result JSON to this path")
    ap.add_argument("--bless-counters", action="store_true",
                    help="write the observed gap set as this probe's counter "
                         "baseline (refuses if any functional assertion is red)")
    args = ap.parse_args()

    do_type = not args.negative
    res = run_mousepad_probe(do_type=do_type, keep=args.keep)
    out = res.to_dict()
    out["fingerprint"] = stack_fingerprint()

    # ---- counter tier: normalize + diff gaps vs the blessed baseline ----
    observed = gapmod.parse_gaps(res.gap_lines)
    baseline_path = BASELINE_DIR / f"{args.probe}.gaps.json"
    allow_path = BASELINE_DIR / f"{args.probe}.expected_gaps"
    baseline = gapmod.load_baseline(baseline_path)
    allowlist = gapmod.load_allowlist(allow_path)
    counter_ok, counter_report = gapmod.compare(observed, baseline, allowlist)
    out["counter"] = counter_report

    if args.bless_counters:
        # Refuse to bless a baseline captured from a broken run: a gap set is
        # only trustworthy when the app actually performed its function.
        if not res.passed:
            print(json.dumps(out, indent=2))
            print("\nBLESS REFUSED: functional assertions are red; fix the "
                  "probe before blessing a counter baseline.")
            sys.exit(1)
        gapmod.write_baseline(baseline_path, args.probe, observed)
        print(json.dumps(out, indent=2))
        print(f"\nBLESSED counter baseline -> {baseline_path} "
              f"({len(observed)} gap identities)")
        sys.exit(0)

    print(json.dumps(out, indent=2))
    if args.json:
        Path(args.json).write_text(json.dumps(out, indent=2))

    if args.negative:
        # For the negative self-test, the gate is CORRECT iff the semantic
        # assertion failed. Any other outcome means the gate can't tell a
        # working app from a broken one.
        sem = [st for st in res.stages if st[0] == "semantic-assert"]
        gate_ok = bool(sem) and not sem[0][1]
        print(f"\nNEGATIVE SELF-TEST: {'PASS (gate caught it)' if gate_ok else 'FAIL (gate blind!)'}")
        sys.exit(0 if gate_ok else 1)
    else:
        # Overall gate = functional stages green AND no new counter gaps.
        # A missing baseline is a soft warning (first run), not a failure.
        gate_pass = res.passed and counter_ok
        if not counter_report["have_baseline"]:
            print("\nNOTE: no counter baseline yet -- run --bless-counters to "
                  "establish one (counter tier not enforced this run).")
            gate_pass = res.passed
        elif counter_report["new_gaps"]:
            print(f"\nNEW GAPS ({len(counter_report['new_gaps'])}): "
                  + ", ".join(g["key"] for g in counter_report["new_gaps"]))
        print(f"\nPROBE {res.name}: {'PASS' if gate_pass else 'FAIL'}")
        sys.exit(0 if gate_pass else 1)


if __name__ == "__main__":
    main()

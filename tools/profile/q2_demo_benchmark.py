#!/usr/bin/env python3
"""Time Brook's two licensed Quake demos through the VM bridge.

Requires the original brook-benchmark.cfg from the experiment seed. This tool
ships no game data. Run in a fresh guest with no other Quake process running.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/vm_bridge"))
import server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session")
    parser.add_argument("--runs", type=int, default=6)
    parser.add_argument("--timeout", type=float, default=420)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.runs <= 20 or not 1 <= args.timeout <= 1200:
        parser.error("runs must be 1–20 and timeout 1–1200 seconds")
    data = server.load_session(args.session)
    log = Path(data["serial_log"])
    ready_deadline = time.monotonic() + 90
    while "DEBUG_TCP: listening on port 1234" not in log.read_text(errors="replace"):
        if time.monotonic() >= ready_deadline:
            parser.error("Guest debug channel did not become ready within 90 seconds")
        time.sleep(1)
    processes = server.guest(data, "procs").get("reply", "")
    if "'quake2'" in processes:
        parser.error("Quake is already running; use a fresh guest for this benchmark")
    offset = log.stat().st_size
    start = time.monotonic()
    server.guest_run(args.session,
        "run --uid 1000 /nix/bin/brook-yquake2-gl gl3 "
        "+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 "
        "+set vid_fullscreen 0 +set cl_showfps 1 "
        "+set timedemo 1 +exec brook-benchmark.cfg")
    samples = []
    while time.monotonic() - start < args.timeout:
        with log.open("rb") as source:
            source.seek(offset)
            text = source.read().decode(errors="replace")
        samples = [
            {"frames": int(frames), "guest_seconds": float(seconds), "fps": float(fps)}
            for frames, seconds, fps in re.findall(
                r"(\d+) frames,\s*([\d.]+) seconds: ([\d.]+) fps", text)
            if int(frames) > 0
        ]
        if len(samples) >= args.runs:
            break
        time.sleep(.2)
    samples = samples[:args.runs]
    for index, sample in enumerate(samples):
        sample["demo"] = "demo1" if index % 2 == 0 else "demo2"
    result = {
        "session": args.session, "kernel_sha256": data["kernel_sha256"],
        "renderer": "gl3", "drawable": [1280, 720],
        "host_elapsed_seconds": time.monotonic() - start,
        "complete": len(samples) == args.runs, "samples": samples,
        "note": "FPS uses the guest clock; host elapsed includes loading. "
                "No sampling or screenshots during timing. Quake remains running.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Take bounded, read-only Dell audio observations during an attended test.

This does not control playback, change volume or launch anything. Give each
run a descriptive label and use a new output path. Host monotonic brackets
show the uncertainty around each group of sequential guest observations.
"""
import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/vm_bridge"))
from hardware import Hardware  # noqa: E402

COMMANDS = ("bridge hardware dell-hda-runtime", "bridge hardware dell-hda-queue",
            "audio stats", "bridge stats")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--samples", type=int, default=4, choices=range(1, 21))
    parser.add_argument("--interval", type=float, default=3)
    args = parser.parse_args()
    if not 1 <= args.interval <= 30:
        parser.error("--interval must be between 1 and 30 seconds")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    hardware = Hardware()
    rows = []
    # Create exclusively before observing; a repeated run cannot erase evidence.
    with args.output.open("x", encoding="utf-8") as output:
        for index in range(args.samples):
            if index:
                time.sleep(args.interval)
            started = time.monotonic_ns()
            try:
                with hardware.session_lock(args.session) as session:
                    replies = hardware.guest_batch(session, COMMANDS)
                    boot_id = session["guest_boot_id"]
                row = {"index": index, "host_start_ns": started,
                       "host_end_ns": time.monotonic_ns(), "boot_id": boot_id,
                       "values": dict(zip(COMMANDS, replies))}
            except Exception as error:
                row = {"index": index, "host_start_ns": started,
                       "host_end_ns": time.monotonic_ns(),
                       "error": f"{type(error).__name__}: {error}"}
            rows.append(row)
            # JSON lines survive an interrupted observation and are independently
            # readable; sequential replies are not simultaneous snapshots.
            output.write(json.dumps({"label": args.label, **row}) + "\n")
            output.flush()
            if "error" in row:
                break
    print(json.dumps({"path": str(args.output), "samples": len(rows),
                      "complete": len(rows) == args.samples and "error" not in rows[-1]}))
    return 0 if len(rows) == args.samples and "error" not in rows[-1] else 1


if __name__ == "__main__":
    raise SystemExit(main())

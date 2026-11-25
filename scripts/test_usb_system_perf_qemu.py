#!/usr/bin/env python3
"""Measure the mounted USB system extent and file I/O on disposable QEMU media."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import time
import uuid

import test_usb_system_qemu as fixture


def timing_delta(before, after):
    return {key: after["timing"][key] - before["timing"][key]
            for key in ("commands", "data_bytes", "errors", "direct_bytes", "uas_flights")
            if key in before["timing"]}


def measure_job(guest, command):
    before = guest.status()
    start = time.monotonic_ns()
    result = guest.job(command)
    elapsed = time.monotonic_ns() - start
    return {"host_elapsed_ns": elapsed, "transport": timing_delta(before, guest.status()),
            "stdout": result.get("stdout", {}).get("text", ""),
            "stderr": result.get("stderr", {}).get("text", "")}


def bench_command(guest, command):
    for attempt in range(3):
        try:
            return guest.call("hw_command", command=command)
        except RuntimeError as error:
            if "timed out" not in str(error) or attempt == 2:
                raise
            # Read-only measurement. Wait for the single bridge worker to
            # finish a possibly dispatched read before issuing a fresh one.
            time.sleep(1 + attempt)


def trim_evidence(guest, mode, output):
    prefix = "USB_SYSTEM_TRIM"
    events = []
    for line in guest.serial.read_text(errors="replace").splitlines():
        if line.startswith((prefix + " ", prefix + "_RANGE ")):
            events.append(json.loads(line.split(" ", 1)[1]))
    done = [row for row in events if row.get("state") == "done"]
    ranges = [row for row in events if "lba" in row]
    assert len(done) == 1 and done[0]["mode"] == mode and done[0]["error"] == 0 and ranges, events
    assert sum(row["sectors"] for row in ranges) == done[0]["sectors"]
    for row in ranges:
        assert row["mode"] == mode and row["sectors"] > 0
        assert fixture.STORE_FIRST <= row["lba"]
        assert row["lba"] + row["sectors"] <= fixture.BACKUP_FIRST
    (output / "trim.jsonl").write_text("".join(json.dumps(row) + "\n" for row in events))
    return done[0]


def browser(guest):
    catalog = guest.call("hw_apps")
    app = next((row for row in catalog["apps"]
                if "chromium" in row["title"].lower() and row["launch_eligible"]), None)
    assert app, "fixture needs an installed Chromium launch target"
    request = uuid.uuid4().hex
    (guest.base / "chromium-request").write_text(request)
    before = guest.status()
    start = time.monotonic_ns()
    result = guest.call("hw_launch", app=app["reference"], request_id=request,
                        wait_for="window", wait_s=4)
    while not result.get("wait_condition_met") and time.monotonic_ns() - start < 90_000_000_000:
        assert result["state"] not in ("failed", "rejected", "lost"), result
        result = guest.call("hw_ui", operation_id=request, wait_for="window", wait_s=4)
    elapsed = time.monotonic_ns() - start
    assert result.get("wait_condition_met"), result
    return {"host_elapsed_ns": elapsed, "transport": timing_delta(before, guest.status())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--store", type=Path, required=True)
    parser.add_argument("--operator-seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cold-file", required=True, help="guest absolute regular store file with at least 64 MiB")
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--skip-browser", action="store_true")
    parser.add_argument("--uas", action="store_true")
    parser.add_argument("--trim-mode", choices=("dry-run", "run"))
    parser.add_argument("--sequential-mib", type=int, choices=(4, 8, 16), default=16)
    parser.add_argument("--random-mib", type=int, choices=(1, 2, 4), default=4)
    args = parser.parse_args()
    if not args.cold_file.startswith("/nix/store/"):
        parser.error("--cold-file must be under /nix/store")
    if args.accel == "kvm" and subprocess.run(["pgrep", "-x", "vmware-vmx"], capture_output=True).returncode == 0:
        raise SystemExit("Workstation is running")
    args.output.mkdir(mode=0o700, parents=True)
    fixture.adopt_prebuilt(args.files)
    system, fallback = fixture.make_disks(args)
    protected = fixture.digest_outside_system(system), fixture.sha(fallback)
    result = {"outside_p2_sha256": protected[0], "cold_file": args.cold_file}
    output = args.output / "verdict.json"
    def record():
        output.write_text(json.dumps(result, indent=2) + "\n")

    guest = fixture.Guest(args, system, fallback, "transport-and-ext2")
    try:
        guest.wait_log("USB_SYSTEM_READY")
        if args.uas:
            guest.wait_log("xhci: UAS ready")
        guest.attach()
        assert guest.status()["mounted"]
        if args.trim_mode:
            result["trim"] = trim_evidence(guest, args.trim_mode, args.output)
        rows = result["raw"] = {}
        sequential = [("seq", size, args.sequential_mib) for size in (4, 16, 64, 256, 1024, 4096)]
        for mode, kib, mib in sequential + [("rand", 4, args.random_mib)]:
            row = bench_command(guest, f"bridge hardware usb-system-bench {mode} {kib} {mib}")
            assert row["bytes"] == mib * (1 << 20) and row["errno"] == 0 and row["duration_ns"] > 0, row
            rows[f"{mode}-{kib}"] = row
            record()
        if args.uas:
            assert guest.status()["timing"]["uas_peak_depth"] == 4
        record()
        dd = "/nix/profile/bin/dd"
        command = f'{dd} if={shlex.quote(args.cold_file)} of=/dev/null bs=1M count=64 status=none'
        result["ext2_cold_read_64m"] = measure_job(guest, command)
        result["ext2_repeat_read_64m"] = measure_job(guest, command)
        record()
        scratch = f"/data/home/.usbbench-{uuid.uuid4().hex[:12]}"
        guest.job(f"umask 077; /nix/profile/bin/mkdir {scratch}")
        try:
            result["ext2_scratch_write_16m"] = measure_job(
                guest, f"cd {scratch} && {dd} if=/dev/zero of=test bs=1M count=16 conv=fsync status=none")
            guest.job(f'cd {scratch} && test "$(/nix/profile/bin/stat -c %s test)" = 16777216')
        finally:
            guest.job(f"cd {scratch} && /nix/profile/bin/rm -f test && cd .. && /nix/profile/bin/rmdir {scratch}")
        record()
        guest.restart()
    finally:
        guest.stop()
    assert (fixture.digest_outside_system(system), fixture.sha(fallback)) == protected
    if not args.skip_browser:
        guest = fixture.Guest(args, system, fallback, "chromium-cold-boot")
        try:
            guest.wait_log("USB_SYSTEM_READY")
            guest.attach()
            try:
                result["chromium_cold_launch"] = browser(guest)
            except (AssertionError, RuntimeError) as error:
                result["chromium_cold_launch"] = {"error": str(error)}
            guest.restart()
        finally:
            guest.stop()
        assert (fixture.digest_outside_system(system), fixture.sha(fallback)) == protected
    result["outside_p2_unchanged"] = True
    record()
    print("USB_SYSTEM_PERF_PASS", output)


if __name__ == "__main__":
    main()

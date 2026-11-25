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
            for key in ("commands", "data_bytes", "errors")}


def measure_job(guest, command):
    before = guest.status()
    start = time.monotonic_ns()
    result = guest.job(command)
    elapsed = time.monotonic_ns() - start
    return {"host_elapsed_ns": elapsed, "transport": timing_delta(before, guest.status()),
            "stdout": result.get("stdout", {}).get("text", ""),
            "stderr": result.get("stderr", {}).get("text", "")}


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
        guest.attach()
        assert guest.status()["mounted"]
        rows = {}
        for mode, kib, mib in [("seq", size, 16) for size in (4, 16, 64, 256, 1024, 4096)] + [("rand", 4, 4)]:
            row = guest.call("hw_command", command=f"bridge hardware usb-system-bench {mode} {kib} {mib}")
            assert row["bytes"] == mib * (1 << 20) and row["errno"] == 0 and row["duration_ns"] > 0, row
            rows[f"{mode}-{kib}"] = row
        result["raw"] = rows
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

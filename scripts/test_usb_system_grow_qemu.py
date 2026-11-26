#!/usr/bin/env python3
"""BROOK_SYSTEM after the grow to ~470 GiB, through the usb-system path, in QEMU.

Run in nix-shell. --disk is a disposable sparse Lexar-layout disk whose p2 was
grown by grow-brook-ssd-system.sh and whose p1 holds --files (a set built with
--usb-system on the grown ssd-system.json, installed by write-brook-ssd-boot.sh);
the rehearsal is rtx-smoke/grow-rehearsal/rehearse.py. The disk is modified.

  write     the groups below the old 128 GiB end are marked full in their
            descriptors, so the guest's first-fit allocator must place a
            --write-mib file past the old boundary; restart cleanly
  readback  a fresh boot reads the file back (sha256), then QEMU is killed:
            BROOK_SYSTEM stays not clean, as after a power cut
  repair    the boot-time e2fsck repairs it (timed from the serial log), the
            guest mounts and reads the file back again; restart cleanly

Afterwards the host checks every data block is past the old end, e2fsck -fn
is clean, and the GPT (apart from p2), p1's files, p3 and the backup GPT are
unchanged across the boots. Uses the debug bridge only against this QEMU guest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_usb_system_qemu as base  # noqa: E402
import test_usb_system_fsck_qemu as fsck  # noqa: E402

SECTOR = 512
OLD_BLOCKS = 268435456 // 8          # the 128 GiB extent before the grow
GROUP_BLOCKS = 32768
TEST_FILE = "/data/home/brook-grow-test.bin"   # /nix/home/brook on BROOK_SYSTEM
EXT2_FILE = "/home/brook/brook-grow-test.bin"


def p2_spec(disk):
    return f"{disk}?offset={base.STORE_FIRST * SECTOR}"


def debugfs(disk, command, write=False):
    return subprocess.run(["debugfs", *(["-w"] if write else []), "-R", command, p2_spec(disk)],
                          capture_output=True, text=True, check=True).stdout


def gpt_lines(disk):
    dump = subprocess.check_output(["sfdisk", "--dump", str(disk)], text=True)
    return {line.split(":")[0].strip()[-1]: line.split(":", 1)[1].strip()
            for line in dump.splitlines() if line.startswith(str(disk))}


def p3_digest(disk):
    entry = gpt_lines(disk)["3"]
    start = int(entry.split("start=")[1].split(",")[0])
    size = int(entry.split("size=")[1].split(",")[0])
    return fsck.store_digest(disk, start * SECTOR, size * SECTOR)


def hide_low_groups(disk):
    """Report zero free blocks in every group below the old end. Only the
    counters change; e2fsck -p corrects them during the repair boot."""
    offset = base.STORE_FIRST * SECTOR
    with disk.open("r+b") as f:
        f.seek(offset + 1024)
        sb = bytearray(f.read(1024))
        blocks, = struct.unpack_from("<I", sb, 4)
        free, = struct.unpack_from("<I", sb, 12)
        groups = (blocks + GROUP_BLOCKS - 1) // GROUP_BLOCKS
        f.seek(offset + 4096)
        gdt = bytearray(f.read(groups * 32))
        hidden = 0
        for g in range(OLD_BLOCKS // GROUP_BLOCKS):
            hidden += struct.unpack_from("<H", gdt, g * 32 + 12)[0]
            struct.pack_into("<H", gdt, g * 32 + 12, 0)
        struct.pack_into("<I", sb, 12, free - hidden)
        f.seek(offset + 4096)
        f.write(gdt)
        f.seek(offset + 1024)
        f.write(sb)
    return {"groups": groups, "hidden_groups": OLD_BLOCKS // GROUP_BLOCKS, "hidden_free_blocks": hidden}


def job(guest, script, timeout):
    request = uuid.uuid4().hex
    with (guest.base / "requests.jsonl").open("a") as log:
        log.write(json.dumps({"request_id": request, "script": script}) + "\n")
    result = guest.call("hw_guest_run", request_id=request, argv=["/nix/profile/bin/bash", "-c", script], wait_s=4)
    deadline = time.monotonic() + timeout
    while result["state"] != "completed" and time.monotonic() < deadline:
        result = guest.call("hw_job", operation_id=result["operation_id"], wait_s=4)
    assert result["state"] == "completed" and result["exit"]["code"] == 0, result
    return result


def job_text(result):
    out = result.get("stdout") or {}
    return out.get("text", "") if isinstance(out, dict) else str(out)


def ready(guest, timeout):
    guest.wait_log(f"groups={base.STORE_SECTORS // 8 // GROUP_BLOCKS}", timeout=timeout)
    guest.attach()
    status = guest.status()
    assert status["stage"] == "ready" and status["mounted"] and status["window_open"], status
    return status


def sha_in_guest(guest):
    out = job_text(job(guest, f"sha256sum {TEST_FILE}", 600))
    return out.split()[0] if out.split() else out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--disk", type=Path, required=True, help="the grown fixture disk (modified)")
    parser.add_argument("--operator-seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--write-mib", type=int, default=2048)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--baseline", type=Path, help="untouched copy of --disk (e.g. a reflink taken before the "
                        "run): GPT, outside-p2 and p3 digests are taken from it")
    parser.add_argument("--resume-after-write", action="store_true",
                        help="the write boot already completed on --disk; start at readback")
    args = parser.parse_args()
    args.output.mkdir(mode=0o700, parents=True)
    base.adopt_prebuilt(args.files)
    record = base.PREBUILT
    groups = base.STORE_SECTORS // 8 // GROUP_BLOCKS
    disk = args.disk
    results = {"record": record, "groups": groups}
    # Brook's view ends at p2; everything after it (gap, p3, backup GPT) is "outside".
    gpt = gpt_lines(disk)
    first, sectors = (int(v) for v in record.splitlines()[2].split(","))
    assert int(gpt["2"].split("start=")[1].split(",")[0]) == first, gpt["2"]
    assert int(gpt["2"].split("size=")[1].split(",")[0]) == sectors, gpt["2"]
    if not args.resume_after_write:
        pre = subprocess.run(["e2fsck", "-fn", p2_spec(disk)], capture_output=True, text=True)
        assert pre.returncode == 0, pre.stdout
        results["hidden"] = hide_low_groups(disk)
    fallback = args.output / "fallback.raw"
    fallback_esp = args.output / "fallback.esp"
    fsck.prepare_esp(args.files, fallback_esp)
    with fallback.open("xb") as f:
        f.truncate((base.ESP_FIRST + base.ESP_SECTORS + 2048) * SECTOR)
    fsck.run("sfdisk", "--no-reread", "--no-tell-kernel", fallback, text=True, capture_output=True,
             input=f'label: gpt\nunit: sectors\n\nstart={base.ESP_FIRST},size={base.ESP_SECTORS},type=U,name="BROOKBOOT"\n')
    base.clone_sparse(fallback_esp, fallback, base.ESP_FIRST * SECTOR)
    fallback_esp.unlink()
    reference = args.baseline or disk
    gpt = gpt_lines(reference)
    gpt["2"] = gpt_lines(disk)["2"]
    outside = base.digest_outside_system(reference)
    p3 = p3_digest(reference)
    results.update({"gpt_p1": gpt["1"], "gpt_p3": gpt["3"], "outside_p2_sha256": outside, "p3_sha256": p3})

    if not args.resume_after_write:
        guest = base.Guest(args, disk, fallback, "write")
        try:
            results["write_status"] = ready(guest, args.timeout)
            job(guest, "command -v dd sha256sum sync", 60)
            started = time.monotonic()
            job(guest, f"dd if=/dev/urandom of={TEST_FILE} bs=1M count={args.write_mib} && sync", 3600)
            results["write_seconds"] = round(time.monotonic() - started, 1)
            results["sha256_written"] = sha_in_guest(guest)
            guest.restart()
        finally:
            guest.stop()

    stat = debugfs(disk, f"stat {EXT2_FILE}")
    blocks = [int(b) for b in debugfs(disk, f"blocks {EXT2_FILE}").split()]
    size = int(stat.split("Size: ")[1].split()[0])
    assert size == args.write_mib << 20, stat
    # debugfs lists data and indirect blocks: 256 data blocks per MiB plus the
    # single, double and leaf indirect blocks.
    indirect = 1 + 1 + (args.write_mib * 256 - 12 - 1024 + 1023) // 1024
    assert len(blocks) == args.write_mib * 256 + indirect and min(blocks) >= OLD_BLOCKS, (len(blocks), min(blocks))
    host = hashlib.sha256(subprocess.run(["debugfs", "-R", f"cat {EXT2_FILE}", p2_spec(disk)],
                                         capture_output=True, check=True).stdout).hexdigest()
    results.setdefault("sha256_written", host)   # resumed: the guest's value is in the earlier log
    assert host == results["sha256_written"], (host, results["sha256_written"])
    results["blocks"] = {"count": len(blocks), "min": min(blocks), "max": max(blocks), "old_end": OLD_BLOCKS,
                         "min_group": min(blocks) // GROUP_BLOCKS, "max_group": max(blocks) // GROUP_BLOCKS,
                         "host_sha256": host}

    guest = base.Guest(args, disk, fallback, "readback")
    try:
        ready(guest, args.timeout)
        results["sha256_readback"] = sha_in_guest(guest)
        assert results["sha256_readback"] == results["sha256_written"]
        guest.process.kill()          # power loss: the dirty marker stays
        guest.process.wait(timeout=10)
    finally:
        guest.stop()
    assert fsck.get_state(disk, base.STORE_FIRST * SECTOR) == 0, "expected a not-clean BROOK_SYSTEM"
    results["state_after_power_loss"] = 0

    guest = base.Guest(args, disk, fallback, "repair")
    try:
        begin = None
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            text = guest.serial.read_text(errors="replace") if guest.serial.exists() else ""
            if begin is None and "running /boot/E2FSCK.ELF -p -f" in text:
                begin = time.monotonic()
            if "USB_SYSTEM_FSCK exit=" in text:
                break
            assert guest.process.poll() is None, "guest exited during repair"
            time.sleep(0.25)
        end = time.monotonic()
        assert begin is not None, "e2fsck never started"
        results["fsck_seconds"] = round(end - begin, 1)
        status = ready(guest, args.timeout)
        log = guest.serial.read_text(errors="replace")
        line = next(l for l in log.splitlines() if "USB_SYSTEM_FSCK exit=" in l)
        results["fsck_line"] = line[line.index("USB_SYSTEM_FSCK"):]
        assert "action=mount" in line and status["fsck"] == "repaired", (line, status)
        results["transcript"] = [l[l.index("e2fsck| ") + 8:] for l in log.splitlines() if "e2fsck| " in l]
        results["sha256_after_repair"] = sha_in_guest(guest)
        assert results["sha256_after_repair"] == results["sha256_written"]
        job(guest, f"rm {TEST_FILE} && sync", 600)   # leave the store as it was
        guest.restart()
    finally:
        guest.stop()

    check = subprocess.run(["e2fsck", "-fn", p2_spec(disk)], capture_output=True, text=True)
    results["host_e2fsck_fn"] = {"exit": check.returncode, "summary": check.stdout.strip().splitlines()[-1]}
    assert check.returncode == 0, check.stdout
    after = gpt_lines(disk)
    assert after["1"] == gpt["1"] and after["3"] == gpt["3"] and after["2"] == gpt["2"], after
    assert base.digest_outside_system(disk) == outside, "bytes outside p2 changed"
    assert p3_digest(disk) == p3, "p3 changed"
    results["outside_p2_unchanged"] = results["gpt_unchanged"] = results["p3_unchanged"] = True
    for case in ("write", "readback", "repair"):
        src = args.output / case / "serial.log"
        if src.exists():
            src.rename(args.output / f"{case}.serial.log")
    fallback.unlink()
    (args.output / "verdict.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"PASS {args.output / 'verdict.json'}")


if __name__ == "__main__":
    main()

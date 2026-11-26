#!/usr/bin/env python3
"""Boot-time BROOK_SYSTEM repair through the usb-system path, in QEMU only.

Run in nix-shell. Supply a --usb-system file set (build_rtx_smoke_files.py)
whose /etc/usb-system names a small extent, e.g. first_lba 198656 and
524288 sectors (256 MiB). The script builds a new disposable GPT disk around
that record with a minimal classic-ext2 store, then boots it per case:

  clean        clean store: mounts with no check (regression)
  not-clean    s_state 0, as after a power cut: e2fsck repairs, then mounts
  bitmap       not-clean plus a cleared block-bitmap bit: repaired, mounts
  unfixable    not-clean plus a root inode that is not a directory: e2fsck
               exits 4 or more and the refusal is kept
  opt-out      not-clean with /etc/usb-system-no-fsck: refused, no check
  extent       not-clean with a record extent that differs from the GPT
               entry: refused before any check, p2 byte-identical

Every case checks that the GPT, p1 files (except loader breadcrumbs), the
p3 Backup partition and the read-only fallback disk are unchanged. Repaired
stores must then pass an independent host e2fsck -fn and carry the
transcript in /nix/var/log/brook/usb-system-fsck.log. Serial only: the
debug bridge is not used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_usb_system_qemu as base  # noqa: E402
from build_rtx_smoke_files import NAMES, fat_write, stick_set  # noqa: E402

SECTOR = 512
MIB = 1 << 20
CASES = ("clean", "not-clean", "bitmap", "unfixable", "opt-out", "extent")


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def make_store(path, fs_uuid, blocks, scratch):
    """Classic ext2 exactly as LargeExt2ClassifyHeader admits it, with the
    minimal layout WritableDesktopPublishDirect checks."""
    tree = scratch / "tree"
    dirs = ["store", "home/brook", "var/cache/narinfo", "profile/bin", "profile/lib",
            "profile/share/applications", "share/applications", "index", "bin"]
    files = {"profile/manifest.tsv": "fixture\n", "index/packages.idx": "fixture\n",
             "share/applications/applications.idx": "fixture\n"}
    executables = ["bin/nix", "bin/nix-fetch", "bin/nar-unpack",
                   "profile/bin/brook-chromium", "profile/bin/brook-gallery"]
    for d in dirs:
        (tree / d).mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (tree / name).write_text(text)
    for name in executables:
        (tree / name).write_text("#!/bin/false\n")
        (tree / name).chmod(0o755)
    run("mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096", "-I", "128", "-m", "0",
        "-O", "none,filetype,sparse_super,large_file", "-U", str(fs_uuid),
        "-E", "root_owner=1000:1000", "-d", tree, path, str(blocks))
    paths = sorted({str(p.relative_to(tree)) for p in tree.rglob("*")})
    script = "".join(f"sif /{p} uid 1000\nsif /{p} gid 1000\n" for p in paths)
    run("debugfs", "-w", "-f", "-", path, input=script.encode(), capture_output=True)
    check = subprocess.run(["e2fsck", "-fn", path], capture_output=True, text=True)
    assert check.returncode == 0, check.stdout + check.stderr


def debugfs(image, command):
    return subprocess.run(["debugfs", "-R", command, str(image)], capture_output=True, text=True,
                          check=True).stdout


def set_state(disk, offset, state):
    with disk.open("r+b") as f:
        f.seek(offset + 1024 + 58)
        f.write(struct.pack("<H", state))


def get_state(disk, offset):
    with disk.open("rb") as f:
        f.seek(offset + 1024 + 58)
        return struct.unpack("<H", f.read(2))[0]


def copy_range(src_path, src_offset, dst_path, dst_offset, size):
    """Sparse copy: only allocated source extents are written."""
    with src_path.open("rb") as src, dst_path.open("r+b") as dst:
        at, end = src_offset, src_offset + size
        while at < end:
            try:
                at = os.lseek(src.fileno(), at, os.SEEK_DATA)
            except OSError:
                break
            if at >= end:
                break
            stop = min(os.lseek(src.fileno(), at, os.SEEK_HOLE), end)
            src.seek(at)
            dst.seek(dst_offset + at - src_offset)
            while at < stop:
                data = src.read(min(4 * MIB, stop - at))
                assert data
                dst.write(data)
                at += len(data)


def extract_store(disk, offset, size, target):
    with target.open("wb") as out:
        out.truncate(size)
    copy_range(disk, offset, target, 0, size)


def write_store(disk, offset, image):
    """Replace p2 with the image: punch p2 out first so no stale data remains."""
    size = image.stat().st_size
    run("fallocate", "--punch-hole", "--keep-size", "--offset", offset, "--length", size, disk)
    copy_range(image, 0, disk, offset, size)


def store_digest(disk, offset, size):
    h = hashlib.sha256()
    with disk.open("rb") as f:
        f.seek(offset)
        remaining = size
        while remaining:
            data = f.read(min(4 * MIB, remaining))
            assert data
            h.update(data)
            remaining -= len(data)
    return h.hexdigest()


def prepare_esp(files, output):
    with output.open("xb") as out:
        out.truncate(base.ESP_SECTORS * SECTOR)
    run("mkfs.fat", "-F", "32", "-n", "BROOK_RTX", output, capture_output=True)
    run("mmd", "-i", output, "::EFI", "::EFI/BOOT", "::KERNEL")
    for name in (*NAMES, "manifest.json"):
        run("mcopy", "-i", output, files / name, "::/" + name)


def install_initrd(files, system, scratch, record=None, opt_out=False):
    """Edit the disposable p1 only, keeping the loader's set digest valid."""
    initrd = scratch / "INITRD.IMG"
    shutil.copyfile(files / "INITRD.IMG", initrd)
    if record is not None:
        fat_write(initrd, "ETC/usb-system", record.encode())
    if opt_out:
        fat_write(initrd, "ETC/usb-system-no-fsck", b"1\n")
    manifest = json.loads((files / "manifest.json").read_text())
    new_set = stick_set((files / "KERNEL/BROOK.ELF").read_bytes(), initrd.read_bytes(), manifest["payload_id"])
    volume = f"{system}@@{base.ESP_FIRST * SECTOR}"
    fat_write(volume, "INITRD.IMG", initrd.read_bytes())
    fat_write(volume, "SET.BIN", new_set)


def make_disks(args, scratch):
    record = base.PREBUILT
    lines = record.split("\n")
    p2_guid = uuid.UUID(bytes_le=bytes.fromhex(lines[1]))
    fs_uuid = uuid.UUID(hex=lines[3])
    if args.store:
        store = args.store
        assert store.stat().st_size == base.STORE_SECTORS * SECTOR, "store size differs from the record"
        with store.open("rb") as f:
            f.seek(1024 + 104)
            assert uuid.UUID(bytes=f.read(16)) == fs_uuid, "store ext2 UUID differs from the record"
    else:
        store = args.output / "store.ext2"
        make_store(store, fs_uuid, base.STORE_SECTORS * SECTOR // 4096, scratch)
    system, fallback = args.output / "system.raw", args.output / "fallback.raw"
    esp, fallback_esp = args.output / "system.esp", args.output / "fallback.esp"
    prepare_esp(args.files, esp)
    prepare_esp(args.files, fallback_esp)
    with system.open("xb") as disk:
        disk.truncate((base.BACKUP_FIRST + base.BACKUP_SECTORS + 2048) * SECTOR)
    with fallback.open("xb") as disk:
        disk.truncate((base.ESP_FIRST + base.ESP_SECTORS + 2048) * SECTOR)
    p1 = f'start={base.ESP_FIRST},size={base.ESP_SECTORS},type=U,name="BROOKBOOT"\n'
    p2 = f'start={base.STORE_FIRST},size={base.STORE_SECTORS},type=L,uuid={p2_guid},name="BROOK_SYSTEM"\n'
    p3 = (f'start={base.BACKUP_FIRST},size={base.BACKUP_SECTORS},'
          'type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,name="Backup"\n')
    for path, entries in ((system, p1 + p2 + p3), (fallback, p1)):
        run("sfdisk", "--no-reread", "--no-tell-kernel", path, text=True, capture_output=True,
            input="label: gpt\nunit: sectors\n\n" + entries)
    base.clone_sparse(esp, system, base.ESP_FIRST * SECTOR)
    base.clone_sparse(fallback_esp, fallback, base.ESP_FIRST * SECTOR)
    write_store(system, base.STORE_FIRST * SECTOR, store)
    with system.open("r+b") as disk:
        disk.seek(base.BACKUP_FIRST * SECTOR)
        disk.write(os.urandom(base.BACKUP_SECTORS * SECTOR))
    return system, fallback, store


def corrupt_bitmap(image):
    """Clear the in-use bit of the root directory's data block."""
    block = int(debugfs(image, "blocks /").split()[0])
    out = debugfs(image, "stats")
    bitmap = None
    for line in out.splitlines():
        if line.strip().startswith("Group  0:") and "block bitmap at" in line:
            bitmap = int(line.split("block bitmap at")[1].split(",")[0])
    assert bitmap is not None, out
    with image.open("r+b") as f:
        f.seek(bitmap * 4096 + block // 8)
        byte = f.read(1)[0]
        assert byte & (1 << (block % 8))
        f.seek(bitmap * 4096 + block // 8)
        f.write(bytes([byte & ~(1 << (block % 8))]))
    return {"bitmap_block": bitmap, "cleared_block": block}


def corrupt_root(image):
    """Make the root inode a regular file: e2fsck -p cannot preen that."""
    run("debugfs", "-w", "-R", "sif <2> mode 0100644", image, capture_output=True)


def boot(args, system, fallback, case, expect, timeout=240):
    guest = base.Guest(args, system, fallback, case)
    try:
        for text in expect:
            guest.wait_log(text, timeout=timeout)
        return guest.serial.read_text(errors="replace")
    finally:
        guest.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--operator-seed", type=Path, required=True, help="unused; base.Guest stores it")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--store", type=Path, help="existing grown store image (read-only input) instead of a new "
                        "minimal one; its size and ext2 UUID must match the set's record")
    parser.add_argument("--timeout", type=int, default=240, help="seconds to wait for each expected log line")
    args = parser.parse_args()
    cases = args.case or list(CASES)
    args.output.mkdir(mode=0o700, parents=True)
    base.adopt_prebuilt(args.files)
    assert base.PREBUILT, "the set must carry /etc/usb-system"
    record = base.PREBUILT
    offset, size = base.STORE_FIRST * SECTOR, base.STORE_SECTORS * SECTOR
    scratch = Path(tempfile.mkdtemp(prefix="fsck-qa-", dir=args.output))
    system, fallback, pristine = make_disks(args, scratch)
    fallback_hash = base.sha(fallback)
    results = {"record": record, "cases": {}}

    def reset(state_value=0):
        write_store(system, offset, pristine)
        install_initrd(args.files, system, scratch)
        set_state(system, offset, state_value)

    def offline_check(case):
        copy = args.output / f"{case}.p2.ext2"
        extract_store(system, offset, size, copy)
        check = subprocess.run(["e2fsck", "-fn", str(copy)], capture_output=True, text=True)
        log = debugfs(copy, "cat /var/log/brook/usb-system-fsck.log")
        copy.unlink()
        return check, log

    for case in cases:
        modified = {}
        reset(1 if case == "clean" else 0)
        if case in ("bitmap", "unfixable"):
            image = args.output / "work.ext2"
            extract_store(system, offset, size, image)
            modified = corrupt_bitmap(image) if case == "bitmap" else (corrupt_root(image) or {"root_mode": "0100644"})
            pre = subprocess.run(["e2fsck", "-fn", str(image)], capture_output=True, text=True)
            assert pre.returncode >= 4, pre.stdout
            modified["host_e2fsck_fn_exit_before"] = pre.returncode
            write_store(system, offset, image)
            image.unlink()
            set_state(system, offset, 0)
        if case == "opt-out":
            install_initrd(args.files, system, scratch, opt_out=True)
        if case == "extent":
            magic, guid, extent, fs_uuid = record.splitlines()
            first, sectors = (int(v) for v in extent.split(","))
            install_initrd(args.files, system, scratch,
                           record=f"{magic}\n{guid}\n{first + 8},{sectors - 8}\n{fs_uuid}\n")
        outside = base.digest_outside_system(system)
        refusal = case in ("opt-out", "extent")
        p2_before = store_digest(system, offset, size) if refusal else None
        entry = {"modified": modified}
        if case in ("clean", "not-clean", "bitmap"):
            expect = ["USB_SYSTEM_READY", "lastlog: ready"]
            if case != "clean":
                expect.insert(0, "USB_SYSTEM_FSCK exit=")
                expect.append("usb-system-fsck.log")
            log = boot(args, system, fallback, case, expect, args.timeout)
            entry["fsck_ran"] = "USB_SYSTEM_FSCK" in log
            assert entry["fsck_ran"] == (case != "clean"), case
            assert "USB_SYSTEM_MAINTENANCE" not in log, case
            if case != "clean":
                line = next(l for l in log.splitlines() if "USB_SYSTEM_FSCK exit=" in l)
                entry["fsck_line"] = line[line.index("USB_SYSTEM_FSCK"):]
                assert "action=mount" in line, line
                entry["transcript"] = [l[l.index("e2fsck| ") + 8:] for l in log.splitlines() if "e2fsck| " in l]
                check, kept = offline_check(case)
                entry["host_e2fsck_fn_exit"] = check.returncode
                assert check.returncode == 0, check.stdout
                entry["kept_log"] = kept.splitlines()
                assert kept.startswith("brook usb-system: e2fsck -p -f exit="), kept
        else:
            stage, error = ("gpt-identity", -22) if case == "extent" else ("needs-filesystem-check", -117)
            log = boot(args, system, fallback, case, [f"USB_SYSTEM_MAINTENANCE stage={stage} error={error}"],
                       args.timeout)
            assert "USB_SYSTEM_READY" not in log
            entry["fsck_ran"] = "USB_SYSTEM_FSCK" in log
            if case == "unfixable":
                line = next(l for l in log.splitlines() if "USB_SYSTEM_FSCK exit=" in l)
                entry["fsck_line"] = line[line.index("USB_SYSTEM_FSCK"):]
                assert "action=refuse" in line and int(line.split("exit=")[1].split()[0]) >= 4, line
                entry["reason"] = next(l for l in log.splitlines() if "was not repaired" in l).strip()
            else:
                assert not entry["fsck_ran"], case
                p2_after = store_digest(system, offset, size)
                assert p2_after == p2_before, case
                entry["p2_unchanged"] = True
                if case == "opt-out":
                    assert "automatic repair disabled by /etc/usb-system-no-fsck" in log
            entry["state_after"] = get_state(system, offset)
            entry["refusal"] = f"stage={stage} error={error}"
        assert base.digest_outside_system(system) == outside, case
        assert base.sha(fallback) == fallback_hash, case
        entry["outside_p2_unchanged"] = True
        shutil.copyfile(args.output / case / "serial.log", args.output / f"{case}.serial.log")
        results["cases"][case] = entry
        print(f"{case}: PASS", flush=True)
    shutil.rmtree(scratch)
    for path in (system, fallback, args.output / "system.esp", args.output / "fallback.esp"):
        path.unlink()
    if not args.store:
        pristine.unlink()
    (args.output / "verdict.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"PASS {args.output / 'verdict.json'}")


if __name__ == "__main__":
    main()

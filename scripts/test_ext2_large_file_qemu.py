#!/usr/bin/env python3
"""Boot Brook with a disposable ext2 virtio disk and exercise files above 4 GiB.

The guest (src/tests/test_ext2_large_guest) writes a sparse 6 GiB file with
data at 0, around 4 GiB - 1/4 GiB/4 GiB + 1, the doubly/triply indirect
boundary, 5 GiB and the end; checks stat, SEEK_END, pread, mmap and sendfile
past 4 GiB; hashes the whole file; truncates to 4.5 GiB and 3 GiB; then writes
a dense file (default 5 GiB + 12345 bytes) and appends with O_APPEND.

The host then checks its own model of every hash, e2fsck -fn, debugfs sizes,
and streams both files out of the image with debugfs to hash them
independently of Brook. The sparse image is deleted unless --keep is given.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
KiB, MiB, GiB = 1 << 10, 1 << 20, 1 << 30
TRIPLE_START = (12 + 1024 + 1024 * 1024) * 4096
REGIONS = [
    (0, 64 * KiB),
    (4 * GiB - 2048, 4096),
    (TRIPLE_START - 8 * KiB, 16 * KiB),
    (5 * GiB, 64 * KiB),
    (6 * GiB - 64 * KiB, 64 * KiB),
]


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def make_base():
    out = bytearray(MiB)
    x = 1
    mask = (1 << 64) - 1
    for i in range(MiB):
        x = (x * 6364136223846793005 + 1442695040888963407) & mask
        out[i] = x >> 56
    return bytes(out)


BASE = make_base()


def pattern(offset, length):
    """The guest's content model: BASE, with each 4 KiB block's first 8 bytes
    replaced by its little-endian file offset."""
    start = offset & ~4095
    end = offset + length
    out = bytearray()
    block = start
    while block < end:
        piece = bytearray(BASE[block % MiB:block % MiB + 4096])
        piece[:8] = block.to_bytes(8, "little")
        out += piece
        block += 4096
    return bytes(out[offset - start:offset - start + length])


def chunks(size, dense, chunk=4 * MiB):
    at = 0
    zero = bytes(chunk)
    while at < size:
        n = min(chunk, size - at)
        if dense:
            yield pattern(at, n)
        else:
            data = None
            for offset, length in REGIONS:
                lo, hi = max(at, offset), min(at + n, offset + length, size)
                if lo < hi:
                    if data is None:
                        data = bytearray(n)
                    data[lo - at:hi - at] = pattern(lo, hi - lo)
            yield bytes(data) if data is not None else zero[:n]
        at += n


def digest(stream):
    h = hashlib.sha256()
    total = 0
    for piece in stream:
        h.update(piece)
        total += len(piece)
    return h.hexdigest(), total


def debugfs_hash(image, name):
    proc = subprocess.Popen(["debugfs", "-R", f"cat /{name}", str(image)],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    result = digest(iter(lambda: proc.stdout.read(4 * MiB), b""))
    if proc.wait():
        raise RuntimeError(f"debugfs cat /{name} failed")
    return result


def debugfs_size(image, name):
    out = subprocess.run(["debugfs", "-R", f"stat /{name}", str(image)], check=True,
                         capture_output=True, text=True).stdout
    return int(re.search(r"Size: (\d+)", out).group(1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--build", type=Path, default=ROOT / "build/release")
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--dense-bytes", type=int, default=5 * GiB + 12345)
    parser.add_argument("--image-gib", type=int, default=8)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--keep", action="store_true", help="keep the ext2 image")
    args = parser.parse_args()
    if args.accel == "kvm" and subprocess.run(["pgrep", "-x", "vmware-vmx"]).returncode == 0:
        parser.error("Workstation is running")
    output = args.output.resolve()
    output.mkdir(parents=True)
    guest = output / "BIGQA.ELF"
    run(os.environ["MUSL_CC"], "-static", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
        ROOT / "src/tests/test_ext2_large_guest/main.c", "-o", guest)
    probe = output / "probe"
    run(sys.executable, ROOT / "scripts/make_uefi_probe.py", "--build", args.build,
        "--output", probe, "--display-width", "800", "--display-height", "600",
        env=dict(os.environ, BROOK_INITRD_KIB="8192"))
    initrd = probe / "payload/INITRD.IMG"
    marker = output / "volatile-desktop"
    marker.write_text("1\n")
    startup = output / "INIT.RC"
    startup.write_text("set wm\nset vfb none\nkconsole\n"
                       f"run --console --uid 0 /boot/BIGQA.ELF /bigext2 {args.dense_bytes}\n")
    for path, name in ((marker, "ETC/volatile-desktop"), (startup, "BOOT/INIT.RC"),
                       (guest, "BOOT/BIGQA.ELF")):
        run("mcopy", "-o", "-i", initrd, path, f"::{name}")
    disk = probe / "probe.raw"
    run("mcopy", "-o", "-i", f"{disk}@@1048576", initrd, "::INITRD.IMG")

    # BROOK_SYSTEM's layout: rev 1, filetype/sparse_super/large_file, 4 KiB
    # blocks, 128-byte inodes. The raw image is sparse.
    tree = output / "ext2-root"
    tree.mkdir()
    (tree / "BROOK.MNT").write_text("/bigext2\n")
    ext2 = output / "ext2.raw"
    with ext2.open("wb") as image:
        image.truncate(args.image_gib * GiB)
    run("mkfs.ext2", "-q", "-F", "-b", "4096", "-I", "128",
        "-O", "^resize_inode,^dir_index,^ext_attr,large_file", "-d", tree, ext2,
        stderr=subprocess.DEVNULL)

    variables = output / "VARS.fd"
    shutil.copyfile(os.environ["OVMF_VARS"], variables)
    serial = output / "serial.log"
    command = [
        "qemu-system-x86_64", "-machine", "q35", "-accel", args.accel,
        "-cpu", "host" if args.accel == "kvm" else "max", "-smp", "4", "-m", "2048",
        "-nodefaults", "-display", "none", "-nic", "none", "-no-reboot",
        "-drive", f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
        "-drive", f"if=pflash,format=raw,file={variables}", "-device", "VGA",
        "-device", "qemu-xhci,id=usb",
        "-drive", f"if=none,id=stick,format=raw,readonly=on,file={disk}",
        "-device", "usb-storage,bus=usb.0,drive=stick,bootindex=1",
        "-drive", f"if=none,id=ext2,format=raw,file={ext2}",
        "-device", "virtio-blk-pci,drive=ext2", "-serial", f"file:{serial}",
    ]
    started = time.monotonic()
    lines = []
    try:
        with (output / "qemu.log").open("w") as log:
            vm = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline and vm.poll() is None:
                    if serial.exists():
                        lines = serial.read_text(errors="replace").splitlines()
                        failure = next((l for l in lines if "EXT2_LARGE_FAIL" in l), None)
                        if failure:
                            raise RuntimeError(f"{failure}; see {serial}")
                        if any("EXT2_LARGE_QEMU_PASS" in l for l in lines):
                            break
                    time.sleep(.5)
                else:
                    raise RuntimeError(f"guest did not complete; see {serial}")
                # Let sync() and the last flush settle before a clean QEMU exit.
                time.sleep(3)
            finally:
                if vm.poll() is None:
                    vm.terminate()
                    try:
                        vm.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        vm.kill()
                        vm.wait()
        guest_seconds = time.monotonic() - started
        report = [l[l.index("EXT2_LARGE_"):] for l in lines if "EXT2_LARGE_" in l]
        print("\n".join(report))
        print(f"guest phase {guest_seconds:.0f}s")

        hashes = {}
        for line in report:
            m = re.match(r"EXT2_LARGE_(?:HASH|WROTE) (\S+) size=(\d+) sha256=([0-9a-f]+)", line)
            if m:
                hashes[m.group(1)] = (int(m.group(2)), m.group(3))
        expected = {"sparse-6g": (6 * GiB, False), "sparse-4.5g": (4 * GiB + GiB // 2, False),
                    "sparse-3g": (3 * GiB, False)}
        if args.dense_bytes:
            expected["dense"] = (args.dense_bytes, True)
        for label, (size, dense) in expected.items():
            want = digest(chunks(size, dense))
            if hashes.get(label) != (size, want[0]):
                raise RuntimeError(f"{label}: guest {hashes.get(label)} model {want}")
        print("model hashes match guest")

        fsck = subprocess.run(["e2fsck", "-fn", str(ext2)], capture_output=True, text=True)
        print(fsck.stdout.strip().splitlines()[-1])
        if fsck.returncode:
            raise RuntimeError(f"e2fsck -fn failed ({fsck.returncode}):\n{fsck.stdout}{fsck.stderr}")
        print("e2fsck -fn clean")

        if debugfs_size(ext2, "sparse") != 3 * GiB:
            raise RuntimeError("debugfs: sparse size")
        if debugfs_hash(ext2, "sparse") != (hashes["sparse-3g"][1], 3 * GiB):
            raise RuntimeError("debugfs: sparse content differs")
        print("debugfs sparse: 3 GiB, sha256 matches")
        if args.dense_bytes:
            final = args.dense_bytes + 100
            if debugfs_size(ext2, "dense") != final:
                raise RuntimeError("debugfs: dense size")
            if debugfs_hash(ext2, "dense") != digest(chunks(final, True)):
                raise RuntimeError("debugfs: dense content differs")
            print(f"debugfs dense: {final} bytes, sha256 matches model")
        used = os.stat(ext2).st_blocks * 512
        print(f"image real usage {used / GiB:.2f} GiB")
        print("EXT2_LARGE_HOST_PASS")
    finally:
        if not args.keep and ext2.exists():
            ext2.unlink()


if __name__ == "__main__":
    main()

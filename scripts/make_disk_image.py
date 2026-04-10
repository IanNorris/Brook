#!/usr/bin/env python3
"""Create a FAT16 disk image for virtio-blk testing.

Produces a 4MB raw FAT16 image at <build_dir>/brook_disk.img containing:
  BROOK.MNT       (mount target: /boot)
  BROOK.CFG       (boot config sample)
  DRIVERS/        (driver modules copied from <build_dir>/kernel/drivers/*.mod)
  TEST/HELLO.TXT

Requires: mkfs.fat (dosfstools), mcopy/mmd (mtools).

Usage:
  python3 scripts/make_disk_image.py <build_dir>
"""

import subprocess
import sys
import os
import glob
import tempfile

def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR running: {' '.join(cmd)}", file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        sys.exit(1)
    return result

def main():
    if len(sys.argv) < 2:
        print("Usage: make_disk_image.py <build_dir>", file=sys.stderr)
        sys.exit(1)

    build_dir = sys.argv[1]
    os.makedirs(build_dir, exist_ok=True)
    img_path = os.path.join(build_dir, "brook_disk.img")

    # 16MB raw image formatted as FAT16.
    # mformat -f only accepts standard floppy sizes; use dd+mkfs.fat instead.
    # mkfs.fat 4.x requires at least ~16MB for FAT16.
    DISK_SIZE = 16 * 1024 * 1024
    with open(img_path, "wb") as f:
        f.write(b"\x00" * DISK_SIZE)
    run(["mkfs.fat", "-F", "16", "-n", "BROOKDISK", img_path])

    with tempfile.TemporaryDirectory() as tmp:
        # BROOK.MNT — tells the kernel to mount this volume at /boot
        mnt = os.path.join(tmp, "BROOK.MNT")
        with open(mnt, "w") as f:
            f.write("/boot\n")

        cfg = os.path.join(tmp, "BROOK.CFG")
        with open(cfg, "w") as f:
            f.write("# Brook OS virtio disk config\n")
            f.write("DISK_LABEL=BROOKDISK\n")
            f.write("LOG_LEVEL=INFO\n")

        test_dir = os.path.join(tmp, "TEST")
        os.makedirs(test_dir)
        hello = os.path.join(test_dir, "HELLO.TXT")
        with open(hello, "w") as f:
            f.write("Hello from virtio-blk!\n")

        run(["mcopy", "-i", img_path, mnt, "::BROOK.MNT"])
        run(["mcopy", "-i", img_path, cfg, "::BROOK.CFG"])
        run(["mmd",   "-i", img_path, "::TEST"])
        run(["mcopy", "-i", img_path, hello, "::TEST/HELLO.TXT"])

        # DRIVERS/ — copy any .mod files from build/kernel/drivers/
        mod_dir = os.path.join(build_dir, "kernel", "drivers")
        mod_files = sorted(glob.glob(os.path.join(mod_dir, "*.mod")))
        if mod_files:
            run(["mmd", "-i", img_path, "::DRIVERS"])
            for mod in mod_files:
                mod_name = os.path.basename(mod).upper()
                run(["mcopy", "-i", img_path, mod, f"::DRIVERS/{mod_name}"])
                print(f"  Added module: DRIVERS/{mod_name} ({os.path.getsize(mod)} bytes)")
        else:
            print(f"  No .mod files found in {mod_dir} (run build first)")

        # BIN/ — copy any user-mode test binaries
        bin_names = ["hello_test", "hello_musl"]
        has_bins = any(os.path.exists(os.path.join(build_dir, n)) for n in bin_names)
        if has_bins:
            run(["mmd", "-i", img_path, "::BIN"])
            for name in bin_names:
                bin_path = os.path.join(build_dir, name)
                if os.path.exists(bin_path):
                    dest_name = name.upper()
                    run(["mcopy", "-i", img_path, bin_path, f"::BIN/{dest_name}"])
                    print(f"  Added binary: BIN/{dest_name} ({os.path.getsize(bin_path)} bytes)")

        # DOOM — copy doom binary and WAD file
        doom_bin = os.path.join(build_dir, "doom", "doom")
        doom_wad = os.path.join(build_dir, "doom1.wad")
        if os.path.exists(doom_bin):
            run(["mcopy", "-i", img_path, doom_bin, "::DOOM"])
            print(f"  Added: DOOM ({os.path.getsize(doom_bin)} bytes)")
        if os.path.exists(doom_wad):
            run(["mcopy", "-i", img_path, doom_wad, "::DOOM1.WAD"])
            print(f"  Added: DOOM1.WAD ({os.path.getsize(doom_wad)} bytes)")

    print(f"Disk image written to {img_path}")

if __name__ == "__main__":
    main()



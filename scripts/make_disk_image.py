#!/usr/bin/env python3
"""Create a FAT16 disk image for virtio-blk testing.

Produces a 1MB raw FAT16 image at <build_dir>/brook_disk.img containing:
  BROOK.MNT       (mount target: /boot)
  BROOK.CFG       (boot config sample)
  DRIVERS/        (driver modules copied from <build_dir>/kernel/drivers/*.mod)
  TEST/HELLO.TXT

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

    # 1MB = 1024 KB FAT16 image
    run(["mformat", "-C", "-f", "1024", "-v", "BROOKDISK", "-i", img_path, "::"])

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

    print(f"Disk image written to {img_path}")

if __name__ == "__main__":
    main()



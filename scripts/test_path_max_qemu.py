#!/usr/bin/env python3
"""Boot a disposable FAT initrd, tmpfs and ext2 and copy deep file trees."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--build", type=Path, default=ROOT / "build/release")
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    args = parser.parse_args()
    if args.accel == "kvm" and subprocess.run(["pgrep", "-x", "vmware-vmx"]).returncode == 0:
        parser.error("Workstation is running")
    output = args.output.resolve()
    output.mkdir(parents=True)
    guest = output / "PATHQA.ELF"
    run(os.environ["MUSL_CC"], "-static", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        ROOT / "src/tests/test_path_max_guest/main.c", "-o", guest)
    probe = output / "probe"
    run(sys.executable, ROOT / "scripts/make_uefi_probe.py", "--build", args.build,
        "--output", probe, "--display-width", "800", "--display-height", "600",
        env=dict(os.environ, BROOK_INITRD_KIB="8192"))
    initrd = probe / "payload/INITRD.IMG"
    marker = output / "volatile-desktop"
    marker.write_text("1\n")
    startup = output / "INIT.RC"
    startup.write_text("set wm\nset vfb none\nkconsole\nrun --console --uid 0 /boot/PATHQA.ELF\n")
    for path, name in ((marker, "ETC/volatile-desktop"), (startup, "BOOT/INIT.RC"),
                       (guest, "BOOT/PATHQA.ELF")):
        run("mcopy", "-o", "-i", initrd, path, f"::{name}")
    disk = probe / "probe.raw"
    run("mcopy", "-o", "-i", f"{disk}@@1048576", initrd, "::INITRD.IMG")

    tree = output / "ext2-root"
    tree.mkdir()
    (tree / "BROOK.MNT").write_text("/longext2\n")
    ext2 = output / "ext2.raw"
    with ext2.open("wb") as image:
        image.truncate(64 * 1024 * 1024)
    run("mkfs.ext2", "-q", "-F", "-b", "4096", "-d", tree, ext2)

    variables = output / "VARS.fd"
    shutil.copyfile(os.environ["OVMF_VARS"], variables)
    serial = output / "serial.log"
    command = [
        "qemu-system-x86_64", "-machine", "q35", "-accel", args.accel,
        "-cpu", "host" if args.accel == "kvm" else "max", "-smp", "4", "-m", "1024",
        "-nodefaults", "-display", "none", "-nic", "none", "-no-reboot",
        "-drive", f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
        "-drive", f"if=pflash,format=raw,file={variables}", "-device", "VGA",
        "-device", "qemu-xhci,id=usb",
        "-drive", f"if=none,id=stick,format=raw,readonly=on,file={disk}",
        "-device", "usb-storage,bus=usb.0,drive=stick,bootindex=1",
        "-drive", f"if=none,id=ext2,format=raw,file={ext2}",
        "-device", "virtio-blk-pci,drive=ext2", "-serial", f"file:{serial}",
    ]
    with (output / "qemu.log").open("w") as log:
        vm = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline and vm.poll() is None:
                if serial.exists():
                    lines = serial.read_text(errors="replace").splitlines()
                    failure = next((line for line in lines if line.startswith("PATH_MAX_FAIL")), None)
                    if failure:
                        raise RuntimeError(f"{failure}; see {serial}")
                    if "PATH_MAX_QEMU_PASS" in lines:
                        print("\n".join(line for line in lines if line.startswith("PATH_MAX_")))
                        return
                time.sleep(.1)
            raise RuntimeError(f"Path fixture did not complete; see {serial}")
        finally:
            if vm.poll() is None:
                vm.terminate()
                try:
                    vm.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    vm.kill()
                    vm.wait()


if __name__ == "__main__":
    main()

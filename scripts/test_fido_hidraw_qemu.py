#!/usr/bin/env python3
"""Exercise the FIDO hidraw path with a headless, ephemeral QEMU authenticator."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import time

from test_rtx_smoke_qemu import Qmp

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def wait_for(vm, serial, fragment, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if serial.exists():
            log = serial.read_text(errors="replace")
            for line in log.splitlines():
                if "FIDO_TEST_FAIL" in line or "KERNEL PANIC" in line:
                    raise RuntimeError(f"{line}; see {serial}")
            if fragment in log:
                return log
        if vm.poll() is not None:
            break
        time.sleep(0.1)
    raise RuntimeError(f"Missing {fragment}; see {serial} and qemu.log")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/release")
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=75)
    args = parser.parse_args()
    if subprocess.run(["pgrep", "-x", "vmware-vmx"], stdout=subprocess.DEVNULL).returncode == 0:
        parser.error("Workstation is running")
    if "u2f-emulated" not in subprocess.check_output(
        [args.qemu, "-device", "help"], text=True
    ):
        parser.error("QEMU needs u2f-emulated (u2fEmuSupport = true)")

    output = args.output.resolve()
    output.mkdir(parents=True)
    guest = output / "FIDOTEST.ELF"
    run(os.environ["MUSL_CC"], "-static", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        ROOT / "src/tests/test_fido_hidraw_guest/main.c", "-o", guest)
    probe = output / "probe"
    run("python3", ROOT / "scripts/make_uefi_probe.py", "--build", args.build, "--output", probe,
        "--driver", "xhci", "--usb-read-only",
        env=dict(os.environ, BROOK_INITRD_KIB="8192"))
    initrd = probe / "payload/INITRD.IMG"
    rc = output / "INIT.RC"
    rc.write_text("set wm\nset vfb none\nkconsole\nrun --console --uid 1000 /boot/FIDOTEST.ELF\n")
    hotplug = output / "usb-hotplug"
    hotplug.write_text("1\n")
    for file, dest in ((rc, "BOOT/INIT.RC"), (guest, "BOOT/FIDOTEST.ELF"),
                       (hotplug, "ETC/usb-hotplug")):
        run("mcopy", "-o", "-i", initrd, file, f"::{dest}")
    disk = probe / "probe.raw"
    run("mcopy", "-o", "-i", f"{disk}@@1048576", initrd, "::INITRD.IMG")
    variables = output / "VARS.fd"
    shutil.copyfile(os.environ["OVMF_VARS"], variables)
    serial, sock = output / "serial.log", output / "qmp.sock"
    kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
    qemu = [args.qemu, "-machine", "q35,i8042=off", "-accel", "kvm" if kvm else "tcg",
            "-cpu", "host" if kvm else "max", "-smp", "4", "-m", "2048", "-nodefaults",
            "-drive", f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
            "-drive", f"if=pflash,format=raw,file={variables}", "-device", "VGA",
            "-device", "qemu-xhci,id=usb", "-device", "usb-hub,bus=usb.0,port=2",
            "-drive", f"if=none,id=stick,format=raw,readonly=on,file={disk}",
            "-device", "usb-storage,bus=usb.0,port=1,drive=stick,bootindex=1",
            "-serial", f"file:{serial}", "-display", "none", "-nic", "none", "-no-reboot",
            "-qmp", f"unix:{sock},server=on,wait=off"]
    with (output / "qemu.log").open("w") as log:
        vm = subprocess.Popen(qemu, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(vm, serial, "FIDO_TEST_READY", args.timeout)
            qmp = Qmp(sock)
            qmp.call("device_add", driver="u2f-emulated", bus="usb.0", port="2.1", id="fido-first")
            wait_for(vm, serial, "FIDO_TEST_ROUNDTRIP", args.timeout)
            qmp.call("device_del", id="fido-first")
            wait_for(vm, serial, "FIDO_TEST_UNPLUGGED", args.timeout)
            qmp.call("device_add", driver="u2f-emulated", bus="usb.0", port="2.1", id="fido-next")
            result = wait_for(vm, serial, "FIDO_TEST_PASS", args.timeout)
            assert sum("USB_FIDO_DESCRIPTOR" in line and "match=1" in line
                       for line in result.splitlines()) == 2
            print(next(line for line in result.splitlines() if line.startswith("FIDO_TEST_PASS")))
        finally:
            vm.terminate()
            try:
                vm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                vm.kill()
                vm.wait()


if __name__ == "__main__":
    main()

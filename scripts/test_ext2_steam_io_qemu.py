#!/usr/bin/env python3
"""Measure small regular-file operations and check a disposable ext2 power cut."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid

import test_usb_system_fsck_qemu as store_fixture
import test_usb_system_qemu as usb
from build_rtx_smoke_files import fat_write, loader_config, stick_set

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/release")
    parser.add_argument("--transport", choices=("virtio", "bot", "uas"), required=True)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="tcg")
    parser.add_argument("--option", action="append", default=[], help="QEMU fw_cfg name=value")
    parser.add_argument("--keep-disks", action="store_true")
    parser.add_argument("--usb-stream-io", action="store_true", help="enable the BROOK_SYSTEM readahead and cache marker")
    parser.add_argument("--trace-uas", action="store_true", help="record QEMU command tags and completions")
    args = parser.parse_args()
    if args.trace_uas and args.transport != "uas":
        parser.error("--trace-uas requires --transport uas")
    if args.accel == "kvm" and subprocess.run(["pgrep", "-x", "vmware-vmx"]).returncode == 0:
        parser.error("Workstation is running")
    out = args.output.resolve()
    out.mkdir(parents=True)
    probe = out / "probe"
    drivers = [] if args.transport == "virtio" else ["--driver", "xhci"]
    run("python3", ROOT / "scripts/make_uefi_probe.py", "--build", args.build, *drivers,
        "--output", probe, "--display-width", "800", "--display-height", "600",
        env=dict(os.environ, BROOK_INITRD_KIB="8192"))
    files = probe / "payload"
    binary = out / "STEAMQA.ELF"
    run(os.environ["MUSL_CC"], "-static", "-O2", "-Wall", "-Wextra", "-Werror",
        ROOT / "scripts/workloads/ext2_steam_io.c", "-o", binary)
    initrd = files / "INITRD.IMG"
    fat_write(initrd, "BOOT/STEAMQA.ELF", binary.read_bytes())
    fat_write(initrd, "ETC/volatile-desktop", b"1\n")
    mount = "/bigext2" if args.transport == "virtio" else "/data/home"
    fat_write(initrd, "BOOT/INIT.RC",
              f"set wm\nset vfb none\nkconsole\nrun --console --uid 0 /boot/STEAMQA.ELF {mount}\n".encode())

    disk = out / "ext2.raw"
    disposable = [probe / "probe.raw", disk]
    firmware = probe / "probe.raw"
    data_args = []
    protected = None
    if args.transport == "virtio":
        run("mcopy", "-o", "-i", f"{firmware}@@1048576", initrd, "::INITRD.IMG")
        tree = out / "tree"
        tree.mkdir()
        (tree / "BROOK.MNT").write_text(f"{mount}\n")
        with disk.open("xb") as stream:
            stream.truncate(256 << 20)
        run("mke2fs", "-q", "-F", "-b", "4096", "-I", "128", "-O",
            "none,filetype,sparse_super,large_file", "-d", tree, disk)
        data_args = ["-drive", f"if=none,id=data,format=raw,cache=none,file={disk}",
                     "-device", "virtio-blk-pci,drive=data"]
    else:
        usb.STORE_BYTES = 256 << 20
        usb.STORE_SECTORS = usb.STORE_BYTES // usb.SECTOR
        usb.BACKUP_FIRST = usb.STORE_FIRST + usb.STORE_SECTORS
        fs_uuid = uuid.uuid4()
        part_uuid = uuid.uuid4()
        usb.PREBUILT = f"BRKUSB1\n{part_uuid.bytes_le.hex()}\n{usb.STORE_FIRST},{usb.STORE_SECTORS}\n{fs_uuid.hex}\n"
        run("mmd", "-i", initrd, "::NIX", "::DATA")
        fat_write(initrd, "ETC/usb-system", usb.PREBUILT.encode())
        (files / "BROOK.CFG").write_bytes(loader_config(800, 600))
        if args.usb_stream_io:
            fat_write(initrd, "ETC/ext2-stream-io", b"1\n")
        if args.transport == "uas":
            fat_write(initrd, "ETC/usb-uas", b"1\n")
            fat_write(initrd, "ETC/usb-uas-fixture", b"1\n")
        (files / "SET.BIN").write_bytes(stick_set((files / "KERNEL/BROOK.ELF").read_bytes(),
                                                initrd.read_bytes(), "ext2-steam-io-fixture"))
        (files / "manifest.json").write_text("{}\n")
        args.files, args.store, args.output = files, None, out
        system, fallback, store = store_fixture.make_disks(args, out)
        disposable += [system, fallback, store, out / "system.esp", out / "fallback.esp"]
        protected = usb.digest_outside_system(system), usb.sha(fallback)
        if args.transport == "uas":
            firmware = usb.uas_boot_carrier(out, system, fallback)
            disposable.append(firmware)
            data_args = ["-drive", f"if=none,id=data,format=raw,cache=none,file={system}",
                         "-device", "usb-uas,id=target,bus=usb.0,port=2",
                         "-device", "scsi-hd,bus=target.0,drive=data"]
        else:
            firmware = None
            data_args = ["-drive", f"if=none,id=data,format=raw,cache=none,file={system}",
                         "-device", "usb-storage,bus=usb.0,port=2,drive=data,bootindex=1"]

    variables = out / "vars.fd"
    shutil.copyfile(os.environ["OVMF_VARS"], variables)
    serial = out / "serial.log"
    loader_args = []
    if firmware:
        loader_args = ["-drive", f"if=none,id=boot,format=raw,readonly=on,file={firmware}",
                       "-device", "virtio-blk-pci,drive=boot,bootindex=1"]
    command = ["qemu-system-x86_64", "-machine", "q35", "-accel", args.accel,
               "-cpu", "host" if args.accel == "kvm" else "max", "-smp", "4", "-m", "2048",
               "-nodefaults", "-display", "none", "-nic", "none", "-no-reboot",
               "-drive", f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
               "-drive", f"if=pflash,format=raw,file={variables}", "-device", "VGA",
               "-device", "qemu-xhci,id=usb", "-serial", f"file:{serial}",
               *loader_args, *data_args]
    if args.trace_uas:
        events = out / "trace.events"
        events.write_text("usb_uas_command\nscsi_req_parsed_lba\nscsi_req_dequeue\n")
        command += ["-trace", f"events={events},file={out / 'uas.trace'}"]
    for option in args.option:
        name, value = option.split("=", 1)
        command.extend(("-fw_cfg", f"name={name},string={value}"))
    try:
        with (out / "qemu.log").open("wb") as log:
            vm = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 180
                while vm.poll() is None and time.monotonic() < deadline:
                    lines = serial.read_text(errors="replace") if serial.exists() else ""
                    if "STEAM_IO_FAIL" in lines:
                        raise RuntimeError(f"guest workload failed, see {serial}")
                    if "STEAM_IO_CUT_READY" in lines:
                        break
                    time.sleep(.05)
                else:
                    raise RuntimeError(f"guest workload did not finish, see {serial}")
            finally:
                if vm.poll() is None:
                    vm.kill()
                vm.wait()
        if protected is not None:
            assert (usb.digest_outside_system(system), usb.sha(fallback)) == protected
            assert "USB_SYSTEM_READY" in lines
            if args.transport == "uas":
                assert "xhci: UAS ready" in lines
            store_fixture.extract_store(system, usb.STORE_FIRST * usb.SECTOR, usb.STORE_BYTES, disk)
        with (out / "e2fsck.log").open("wb") as log:
            run("e2fsck", "-fn", disk, stdout=log, stderr=subprocess.STDOUT)
        direct_prefix = "" if args.transport == "virtio" else "/home/brook"

        def readback(name):
            return subprocess.check_output(
                ["debugfs", "-R", f"cat {direct_prefix}/{name}", disk], stderr=subprocess.DEVNULL)

        assert readback("sync.witness") == b"ordered writeback witness\n"
        for i in range(4):
            assert readback(f"pack-{i}.dat") == b"\x39" * (16 << 20)
        report = [dict(field.split("=", 1) for field in line.split("STEAM_IO ", 1)[1].split())
                  for line in lines.splitlines() if "STEAM_IO phase=" in line]
        assert len(report) == 5
        cut_rows = [line.split("STEAM_IO_AT_CUT ", 1)[1].split()
                    for line in lines.splitlines() if "STEAM_IO_AT_CUT " in line]
        queued_bytes = sum(int(row[2]) for row in cut_rows if row[0].isdigit())
        snapshots = {}
        phase = "initial"
        for line in lines.splitlines():
            if "STEAM_IO phase=" in line:
                phase = line.split("phase=", 1)[1].split()[0]
            if "STEAM_IO_DISKSTATS " in line:
                values = line.split("STEAM_IO_DISKSTATS ", 1)[1].split()
                if len(values) >= 10:
                    snapshots.setdefault(phase, {})[values[2]] = {
                        "reads": int(values[3]), "read_sectors": int(values[5]),
                        "writes": int(values[7]), "written_sectors": int(values[9]),
                    }
        verdict = {"transport": args.transport, "phases": report, "diskstats": snapshots,
                   "pending_at_cut_bytes": queued_bytes, "e2fsck": "clean",
                   "o_sync_readback": True, "kernel_sha256": usb.sha(files / "KERNEL/BROOK.ELF")}
        (out / "verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")
        print((out / "verdict.json").read_text())
    finally:
        if not args.keep_disks:
            for path in disposable:
                path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()

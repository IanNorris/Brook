#!/usr/bin/env python3
"""Prove persistent USB system boot on a new disposable 128 GiB GPT fixture.

Run in nix-shell. Supply an RTX desktop file set built for a synthetic
operator and an already-grown ext2 store image. The file set and store
are read-only inputs. The only writable disk is a new regular file.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import tempfile
import time
import uuid
import zlib

from build_rtx_smoke_files import NAMES, fat_write, stick_set
from test_rtx_smoke_qemu import Qmp, Rpc, sha


SECTOR = 512
MIB = 1 << 20
STORE_BYTES = 128 << 30
ESP_FIRST = 2048
ESP_SECTORS = (96 * MIB) // SECTOR
STORE_FIRST = ESP_FIRST + ESP_SECTORS
STORE_SECTORS = STORE_BYTES // SECTOR
BACKUP_FIRST = STORE_FIRST + STORE_SECTORS
BACKUP_SECTORS = 4 * MIB // SECTOR
SENTINEL = "brook-usb-system-persistence"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def clone_sparse(source, target, base):
    with source.open("rb") as src, target.open("r+b") as dest:
        limit = os.fstat(src.fileno()).st_size
        begin = 0
        while begin < limit:
            try:
                begin = os.lseek(src.fileno(), begin, os.SEEK_DATA)
            except OSError as error:
                if error.errno == errno.ENXIO:
                    break
                raise
            end = os.lseek(src.fileno(), begin, os.SEEK_HOLE)
            src.seek(begin)
            dest.seek(base + begin)
            while begin < end:
                data = src.read(min(MIB, end - begin))
                if not data:
                    raise OSError("unexpected short sparse clone")
                dest.write(data)
                begin += len(data)


def prepare_esp(source, output, record=None):
    files = output.with_suffix(".files")
    shutil.copytree(source, files)
    if record is not None and not PREBUILT:
        initrd = files / "INITRD.IMG"
        run("mdel", "-i", initrd, "::/ETC/live-store")
        fat_write(initrd, "ETC/usb-system", record)
        manifest_path = files / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        (files / "SET.BIN").write_bytes(stick_set(
            (files / "KERNEL/BROOK.ELF").read_bytes(), initrd.read_bytes(), manifest["payload_id"]))
        for name in ("INITRD.IMG", "SET.BIN"):
            path = files / name
            manifest["files"][name] = {"bytes": path.stat().st_size, "sha256": sha(path)}
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    with output.open("xb") as out:
        out.truncate(ESP_SECTORS * SECTOR)
    run("mkfs.fat", "-F", "32", *(("-s", "32") if PREBUILT else ()), "-n", "BROOK_RTX", output)
    run("mmd", "-i", output, "::EFI", "::EFI/BOOT", "::KERNEL")
    for name in (*NAMES, "manifest.json"):
        run("mcopy", "-i", output, files / name, "::/" + name)


def digest_backup(path):
    with path.open("rb") as disk:
        disk.seek(BACKUP_FIRST * SECTOR)
        return hashlib.sha256(disk.read(BACKUP_SECTORS * SECTOR)).hexdigest()


def digest_outside_system(path):
    """Hash every byte before and after p2, including the backup GPT."""
    digest = hashlib.sha256()
    with path.open("rb") as disk:
        size = os.fstat(disk.fileno()).st_size
        for start, end in ((0, STORE_FIRST * SECTOR), (BACKUP_FIRST * SECTOR, size)):
            disk.seek(start)
            remaining = end - start
            while remaining:
                data = disk.read(min(MIB, remaining))
                assert data, "short read while hashing protected sectors"
                digest.update(data)
                remaining -= len(data)
    return digest.hexdigest()


def install_test_record(args, system, record):
    """Edit the disposable p1 only and keep the loader's set hash valid."""
    original = args.output / "system.files"
    initrd = args.output / "negative-INITRD.IMG"
    shutil.copyfile(original / "INITRD.IMG", initrd)
    if record is not None:
        fat_write(initrd, "ETC/usb-system", record)
    manifest = json.loads((original / "manifest.json").read_text())
    new_set = stick_set((original / "KERNEL/BROOK.ELF").read_bytes(), initrd.read_bytes(), manifest["payload_id"])
    volume = f"{system}@@{ESP_FIRST * SECTOR}"
    fat_write(volume, "INITRD.IMG", initrd.read_bytes())
    fat_write(volume, "SET.BIN", new_set)


def write_disagreeing_backup_gpt(system):
    """Keep the backup header and entries internally valid, with a different p2 name."""
    with system.open("r+b") as disk:
        header_at = os.fstat(disk.fileno()).st_size - SECTOR
        disk.seek(header_at)
        header = bytearray(disk.read(SECTOR))
        assert header[:8] == b"EFI PART"
        entries_lba, count, entry_size = struct.unpack_from("<QII", header, 72)
        header_size, = struct.unpack_from("<I", header, 12)
        entries_at = entries_lba * SECTOR
        disk.seek(entries_at)
        entries = bytearray(disk.read(count * entry_size))
        original = (header_at, bytes(header), entries_at, bytes(entries))
        assert entries[entry_size + 56] == ord("B"), "expected BROOK_SYSTEM in backup GPT"
        entries[entry_size + 56] = ord("X")
        struct.pack_into("<I", header, 88, zlib.crc32(entries))
        struct.pack_into("<I", header, 16, 0)
        struct.pack_into("<I", header, 16, zlib.crc32(header[:header_size]))
        disk.seek(entries_at)
        disk.write(entries)
        disk.seek(header_at)
        disk.write(header)
    return original


PREBUILT = None


def adopt_prebuilt(files):
    """If the set already carries /etc/usb-system (build_rtx_smoke_files.py
    --usb-system), use it unmodified and lay the fixture out at its extent."""
    global PREBUILT, ESP_SECTORS, STORE_FIRST, STORE_SECTORS, BACKUP_FIRST, STORE_BYTES
    got = subprocess.run(["mtype", "-i", files / "INITRD.IMG", "::/ETC/usb-system"], capture_output=True, text=True)
    if got.returncode:
        return
    lines = got.stdout.split("\n")
    assert lines[0] == "BRKUSB1" and len(lines) == 5 and lines[4] == "", "bad usb-system record"
    first, sectors = (int(v) for v in lines[2].split(","))
    PREBUILT = got.stdout
    ESP_SECTORS = first - ESP_FIRST
    STORE_FIRST, STORE_SECTORS = first, sectors
    STORE_BYTES = sectors * SECTOR
    BACKUP_FIRST = STORE_FIRST + STORE_SECTORS


def make_disks(args):
    for source in (args.store, args.operator_seed):
        if not stat.S_ISREG(source.stat().st_mode):
            raise ValueError(f"{source} must be a regular file")
    assert args.store.stat().st_size == STORE_BYTES, "expected a grown 128 GiB store"
    with args.store.open("rb") as store:
        store.seek(1024 + 104)
        fs_uuid = uuid.UUID(bytes=store.read(16))
    p2_guid = uuid.uuid4()
    record = f"BRKUSB1\n{p2_guid.bytes_le.hex()}\n{STORE_FIRST},{STORE_SECTORS}\n{fs_uuid.hex}\n"
    if PREBUILT:  # a --usb-system production set: build the fixture around its record
        record = PREBUILT
        lines = record.split("\n")
        p2_guid = uuid.UUID(bytes_le=bytes.fromhex(lines[1]))
        assert lines[3] == fs_uuid.hex, "store image ext2 UUID differs from the set's record"
    (args.output / "usb-system.record").write_text(record)
    system = args.output / "system.raw"
    fallback = args.output / "fallback.raw"
    esp = args.output / "system.esp"
    fallback_esp = args.output / "fallback.esp"
    prepare_esp(args.files, esp, record.encode())
    prepare_esp(args.files, fallback_esp)
    with system.open("xb") as disk:
        disk.truncate((BACKUP_FIRST + BACKUP_SECTORS + 2048) * SECTOR)
    with fallback.open("xb") as disk:
        disk.truncate((ESP_FIRST + ESP_SECTORS + 2048) * SECTOR)
    p1 = f'start={ESP_FIRST},size={ESP_SECTORS},type=U,name="BROOKBOOT"\n'
    p2 = f'start={STORE_FIRST},size={STORE_SECTORS},type=L,uuid={p2_guid},name="BROOK_SYSTEM"\n'
    p3 = f'start={BACKUP_FIRST},size={BACKUP_SECTORS},type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,name="Backup"\n'
    for path, entries in ((system, p1 + p2 + p3), (fallback, p1)):
        run("sfdisk", "--no-reread", "--no-tell-kernel", path,
            text=True, input="label: gpt\nunit: sectors\n\n" + entries)
    clone_sparse(esp, system, ESP_FIRST * SECTOR)
    clone_sparse(fallback_esp, fallback, ESP_FIRST * SECTOR)
    clone_sparse(args.store, system, STORE_FIRST * SECTOR)
    with system.open("r+b") as disk:
        disk.seek(BACKUP_FIRST * SECTOR)
        for _ in range(BACKUP_SECTORS * SECTOR // MIB):
            disk.write(os.urandom(MIB))
    return system, fallback


def available_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def uas_boot_carrier(base, system, fallback):
    # OVMF only has a BOT driver. Give firmware a read-only virtio
    # carrier with the same boot partition identity. Brook sees only the
    # UAS device's matching USB GPT entry, just as on the physical SSD.
    carrier = base / "firmware-boot.raw"
    run("cp", "--reflink=auto", "--sparse=always", fallback, carrier)
    guid = subprocess.check_output(["sfdisk", "--part-uuid", system, "1"], text=True).strip()
    run("sfdisk", "--no-reread", "--no-tell-kernel", "--part-uuid", carrier, "1", guid)
    with system.open("rb") as source:
        source.seek(ESP_FIRST * SECTOR)
        with carrier.open("r+b") as dest:
            dest.seek(ESP_FIRST * SECTOR)
            remaining = ESP_SECTORS * SECTOR
            while remaining:
                chunk = source.read(min(MIB, remaining))
                if not chunk:
                    raise OSError("short read from firmware boot partition")
                dest.write(chunk)
                remaining -= len(chunk)
    return carrier


class Guest:
    def __init__(self, args, system, fallback, case):
        base = args.output / case
        base.mkdir()
        self.serial = base / "serial.log"
        self.port = getattr(args, "fixed_port", None) or available_port()
        self.bulk_port = available_port() if getattr(args, "profile_transfer", False) else None
        bulk_forward = f",hostfwd=tcp:127.0.0.1:{self.bulk_port}-:1236" if self.bulk_port else ""
        self.sock_dir = tempfile.TemporaryDirectory(prefix="brook-usb-system-")
        self.sock = Path(self.sock_dir.name) / "qmp.sock"
        variables = base / "VARS.fd"
        shutil.copyfile(os.environ["OVMF_VARS"], variables)
        acceleration = ["-accel", args.accel, "-cpu", "host" if args.accel == "kvm" else "max"]
        self.rpc = None
        self.output = (base / "qemu.log").open("x")
        trace_spec = os.environ.get("BROOK_USB_SYSTEM_QEMU_TRACE")
        trace = ["-trace", f"events={trace_spec},file={base / 'qemu-usb.trace'}"] if trace_spec else []
        if getattr(args, "uas", False):
            carrier = uas_boot_carrier(base, system, fallback)
            storage = ["-drive", f"if=none,format=raw,id=carrier,file={carrier},readonly=on",
                       "-device", "virtio-blk-pci,drive=carrier,bootindex=1",
                       "-device", "usb-uas,id=system-usb,bus=usb.0,port=2",
                       "-device", "scsi-hd,bus=system-usb.0,drive=system"]
        else:
            storage = ["-device", "usb-storage,id=system-usb,bus=usb.0,port=2,drive=system,bootindex=1"]
        self.process = subprocess.Popen(
            ["qemu-system-x86_64", "-machine", "q35,i8042=off", *acceleration, "-smp", "4", "-m", "4096",
             "-nodefaults", "-no-reboot", "-display", "none", "-serial", f"file:{self.serial}", *trace,
             "-qmp", f"unix:{self.sock},server=on,wait=off",
             "-drive", f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
             "-drive", f"if=pflash,format=raw,file={variables}",
             "-device", "VGA,vgamem_mb=64", "-device", "qemu-xhci,id=usb",
             "-drive", f"if=none,format=raw,id=other,file={fallback},readonly=on",
             "-device", "usb-storage,bus=usb.0,port=1,drive=other",
             "-drive", f"if=none,format=raw,id=system,file={system},cache=none,discard=unmap",
             *storage,
             "-device", "usb-net,bus=usb.0,port=3,netdev=net",
             "-netdev", f"user,id=net,restrict=on,hostfwd=tcp:127.0.0.1:{self.port}-:1234{bulk_forward}",
             "-device", "usb-hub,bus=usb.0,port=4,id=hub",
             "-device", "usb-kbd,bus=usb.0,port=4.2,id=kbd"],
            stdout=self.output, stderr=subprocess.STDOUT)
        self.key = args.operator_seed.absolute()
        self.base = base

    def wait_log(self, expected, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.process.poll() is None:
            if self.serial.exists() and expected in self.serial.read_text(errors="replace"):
                return
            time.sleep(0.25)
        raise RuntimeError(f"missing {expected!r} in {self.serial}; check {self.base / 'qemu.log'}")

    def attach(self):
        self.rpc = Rpc(self.base)
        self.rpc.request("initialize", {"protocolVersion": "2025-06-18"})
        deadline = time.monotonic() + 30
        while True:
            try:
                attached = self.rpc.tool("hw_attach", {
                    "host": "127.0.0.1", "port": self.port, "auth_mode": "v3",
                    "operator_key_file": str(self.key)})
                self.session = attached["session"]
                self.restart_prompt_api = attached["guest"].get("restart_prompt_api") == 1
                return attached["guest"]
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(1)

    def call(self, tool, **args):
        return self.rpc.tool(tool, {"session": self.session, **args})

    def status(self):
        return self.call("hw_command", command="bridge hardware caps")["usb_system"]

    def job(self, script):
        request = uuid.uuid4().hex
        with (self.base / "requests.jsonl").open("a") as log:
            log.write(json.dumps({"request_id": request, "script": script}) + "\n")
        job = self.call("hw_guest_run", request_id=request, argv=["/nix/profile/bin/bash", "-c", script], wait_s=4)
        for _ in range(12):
            if job["state"] == "completed":
                break
            job = self.call("hw_job", operation_id=job["operation_id"], wait_s=4)
        assert job["state"] == "completed" and job["exit"]["code"] == 0, job
        return job

    def restart(self):
        request = uuid.uuid4().hex
        (self.base / "restart-id").write_text(request + "\n")
        self.call("hw_power", request_id=request, operation="restart",
                  **({"force": True} if self.restart_prompt_api else {}))
        deadline = time.monotonic() + 60
        while self.process.poll() is None and time.monotonic() < deadline:
            status = self.call("hw_power", request_id=request, operation="status")
            if status["state"] == "ready":
                try:
                    self.call("hw_power", request_id=request, operation="commit")
                except RuntimeError:
                    # Guest reset can close the reply socket.
                    pass
                break
            assert status["state"] != "failed", status
            time.sleep(0.5)
        assert self.process.wait(timeout=20) == 0
        assert "USB_SYSTEM_FINAL_SYNC result=0 clean=1" in self.serial.read_text(errors="replace")

    def stop(self):
        if self.process.poll() is None:
            try:
                Qmp(self.sock).call("quit")
                self.process.wait(timeout=10)
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=10)
        if self.rpc:
            self.rpc.close()
        self.output.close()
        self.sock_dir.cleanup()


def check_refusal(args, system, fallback, case, stage, error):
    outside_before = digest_outside_system(system)
    guest = Guest(args, system, fallback, case)
    try:
        guest.wait_log(f"USB_SYSTEM_MAINTENANCE stage={stage} error={error}")
        boot = guest.attach()
        status = guest.status()
        assert status["stage"] == stage and status["error"] == error
        assert not status["window_open"] and not status["mounted"]
        assert status["writes"] == 0
        log = guest.serial.read_text(errors="replace")
        assert "USB_SYSTEM_READY" not in log and "live-store: ready" not in log
    finally:
        guest.stop()
    assert digest_outside_system(system) == outside_before, case
    return boot, {**status, "outside_p2_sha256": outside_before}


def check_refused_identities(args, system, fallback):
    results = {}
    record = (args.output / "usb-system.record").read_text()
    magic, guid, extent, fs_uuid = record.splitlines()
    records = [
        ("wrong-record-guid", f"{magic}\n{uuid.uuid4().bytes_le.hex()}\n{extent}\n{fs_uuid}\n", "gpt-identity"),
        ("wrong-record-extent", f"{magic}\n{guid}\n{STORE_FIRST + 8},{STORE_SECTORS - 8}\n{fs_uuid}\n", "gpt-identity"),
        ("wrong-record-fs-uuid", f"{magic}\n{guid}\n{extent}\n{uuid.uuid4().hex}\n", "ext2-identity"),
        ("malformed-record", f"BRKUSB0\n{guid}\n{extent}\n{fs_uuid}\n", "record"),
    ]
    try:
        for case, text, stage in records:
            install_test_record(args, system, text.encode())
            _, results[case] = check_refusal(args, system, fallback, case, stage, -22)
    finally:
        install_test_record(args, system, None)

    sfdisk = ("sfdisk", "--no-reread", "--no-tell-kernel")
    linux_data = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
    for case, option, invalid, original in [
        ("wrong-partition-name", "--part-label", "NOT_BROOK_SYSTEM", "BROOK_SYSTEM"),
        ("wrong-partition-type", "--part-type", "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", linux_data),
    ]:
        run(*sfdisk, option, system, "2", invalid)
        try:
            _, results[case] = check_refusal(args, system, fallback, case, "gpt-identity", -22)
        finally:
            run(*sfdisk, option, system, "2", original)
    header_at, header, entries_at, entries = write_disagreeing_backup_gpt(system)
    try:
        _, results["gpt-copy-disagreement"] = check_refusal(
            args, system, fallback, "gpt-copy-disagreement", "gpt-identity", -22)
    finally:
        with system.open("r+b") as disk:
            disk.seek(entries_at)
            disk.write(entries)
            disk.seek(header_at)
            disk.write(header)
    return results


def check_live_detach(args, disk, fallback):
    outside_before = digest_outside_system(disk)
    guest = Guest(args, disk, fallback, "live-detach")
    try:
        guest.wait_log("groups=1024")
        guest.attach()
        assert guest.status()["window_open"]
        Qmp(guest.sock).call("device_del", id="system-usb")
        guest.wait_log("storage 46f4:0001 disconnected")
        fenced = guest.status()
        assert fenced["uncertain"] and not fenced["window_open"] and fenced["dirty"], fenced
        request = uuid.uuid4().hex
        guest.call("hw_power", request_id=request, operation="restart")
        guest.wait_log("USB_SYSTEM_FINAL_SYNC result=-5 clean=0")
        final = guest.status()
        assert final["uncertain"] and final["final_sync_attempted"] and not final["clean"]
        assert final["writes"] == fenced["writes"], (fenced, final)
    finally:
        guest.stop()
    assert digest_outside_system(disk) == outside_before
    _, reboot = check_refusal(args, disk, fallback, "reboot-after-detach", "needs-filesystem-check", -117)
    assert reboot["dirty"]
    return {"fenced": fenced, "final_sync": final, "reboot": reboot}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--store", type=Path, required=True)
    parser.add_argument("--operator-seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exercise-detach", action="store_true", help="also unplug the SSD in a running guest")
    parser.add_argument("--uas", action="store_true", help="use the QEMU USB Attached SCSI device")
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    args = parser.parse_args()
    if args.accel == "kvm" and subprocess.run(["pgrep", "-x", "vmware-vmx"], capture_output=True).returncode == 0:
        raise SystemExit("Workstation is running. Use TCG or wait for the VMware window to close.")
    args.output.mkdir(mode=0o700, parents=True)
    adopt_prebuilt(args.files)
    system, fallback = make_disks(args)
    backup = digest_backup(system)
    outside = digest_outside_system(system)
    fallback_hash = sha(fallback)
    results = {"backup_sha256": backup, "outside_p2_sha256": outside}
    guest = Guest(args, system, fallback, "clean-boot")
    try:
        guest.wait_log("groups=1024")
        results["first_boot"] = guest.attach()
        results["first_status"] = guest.status()
        assert results["first_status"]["dirty"] and results["first_status"]["window_open"]
        assert results["first_status"]["disk"] == 1, "loader must select the non-first USB disk"
        guest.job(f"printf '%s' {SENTINEL} > /data/home/usb-system-test")
        qmp = Qmp(guest.sock)
        qmp.call("device_del", id="kbd")
        guest.wait_log("disconnected (slot")
        qmp.call("device_del", id="hub")
        guest.wait_log("hub 0409:55aa disconnected")
        qmp.call("device_add", driver="usb-hub", bus="usb.0", port="4", id="hub-new")
        guest.job(f"test \"$(< /data/home/usb-system-test)\" = {SENTINEL}")
        guest.restart()
    finally:
        guest.stop()
    assert digest_outside_system(system) == outside
    assert sha(fallback) == fallback_hash
    if args.exercise_detach:
        fault = args.output / "fault.raw"
        with fault.open("xb") as f:
            f.truncate(system.stat().st_size)
        clone_sparse(system, fault, 0)
        results["live_detach"] = check_live_detach(args, fault, fallback)

    guest = Guest(args, system, fallback, "restart-and-power-loss")
    try:
        guest.wait_log("groups=1024")
        results["second_boot"] = guest.attach()
        assert results["second_boot"]["boot_id"] != results["first_boot"]["boot_id"]
        results["second_status"] = guest.status()
        guest.job(f"test \"$(< /data/home/usb-system-test)\" = {SENTINEL}")
        # An abrupt loss must leave the already-flushed dirty marker in place.
        guest.process.kill()
        guest.process.wait(timeout=10)
    finally:
        guest.stop()
    assert digest_backup(system) == backup

    results["dirty_boot"], results["dirty_status"] = check_refusal(
        args, system, fallback, "dirty-boot", "needs-filesystem-check", -117)
    assert results["dirty_status"]["dirty"] and digest_outside_system(system) == outside
    assert sha(fallback) == fallback_hash
    # Duplicate only the p1 identity; the firmware still selected the SSD.
    guid = subprocess.check_output(["sfdisk", "--part-uuid", system, "1"], text=True).strip()
    fallback_guid = subprocess.check_output(["sfdisk", "--part-uuid", fallback, "1"], text=True).strip()
    sfdisk = ("sfdisk", "--no-reread", "--no-tell-kernel", "--part-uuid", fallback, "1")
    run(*sfdisk, guid)
    matching_fallback = sha(fallback)
    _, results["ambiguous_status"] = check_refusal(
        args, system, fallback, "duplicate-boot-volume", "boot-volume-ambiguous", -19)
    assert digest_outside_system(system) == outside
    assert sha(fallback) == matching_fallback
    run(*sfdisk, fallback_guid)
    assert sha(fallback) == fallback_hash

    results["negative_identities"] = check_refused_identities(args, system, fallback)
    assert digest_backup(system) == backup
    assert sha(fallback) == fallback_hash
    results["backup_unchanged"] = True
    (args.output / "verdict.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"PASS {args.output / 'verdict.json'}")


if __name__ == "__main__":
    main()

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
import subprocess
import tempfile
import time
import uuid

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
    if record is not None:
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
    run("mkfs.fat", "-F", "32", "-n", "BROOK_RTX", output)
    run("mmd", "-i", output, "::EFI", "::EFI/BOOT", "::KERNEL")
    for name in (*NAMES, "manifest.json"):
        run("mcopy", "-i", output, files / name, "::/" + name)


def digest_backup(path):
    with path.open("rb") as disk:
        disk.seek(BACKUP_FIRST * SECTOR)
        return hashlib.sha256(disk.read(BACKUP_SECTORS * SECTOR)).hexdigest()


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


class Guest:
    def __init__(self, args, system, fallback, case):
        base = args.output / case
        base.mkdir()
        self.serial = base / "serial.log"
        self.port = available_port()
        self.sock_dir = tempfile.TemporaryDirectory(prefix="brook-usb-system-")
        self.sock = Path(self.sock_dir.name) / "qmp.sock"
        variables = base / "VARS.fd"
        shutil.copyfile(os.environ["OVMF_VARS"], variables)
        acceleration = ["-accel", args.accel, "-cpu", "host" if args.accel == "kvm" else "max"]
        self.rpc = None
        self.output = (base / "qemu.log").open("x")
        self.process = subprocess.Popen(
            ["qemu-system-x86_64", "-machine", "q35,i8042=off", *acceleration, "-smp", "4", "-m", "4096",
             "-nodefaults", "-no-reboot", "-display", "none", "-serial", f"file:{self.serial}",
             "-qmp", f"unix:{self.sock},server=on,wait=off",
             "-drive", f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
             "-drive", f"if=pflash,format=raw,file={variables}",
             "-device", "VGA,vgamem_mb=64", "-device", "qemu-xhci,id=usb",
             "-drive", f"if=none,format=raw,id=other,file={fallback},snapshot=on",
             "-device", "usb-storage,bus=usb.0,port=1,drive=other",
             "-drive", f"if=none,format=raw,id=system,file={system},cache=none",
             "-device", "usb-storage,bus=usb.0,port=2,drive=system,bootindex=1",
             "-device", "usb-net,bus=usb.0,port=3,netdev=net",
             "-netdev", f"user,id=net,restrict=on,hostfwd=tcp:127.0.0.1:{self.port}-:1234",
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
        self.call("hw_power", request_id=request, operation="restart")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--store", type=Path, required=True)
    parser.add_argument("--operator-seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    args = parser.parse_args()
    if args.accel == "kvm" and subprocess.run(["pgrep", "-x", "vmware-vmx"], capture_output=True).returncode == 0:
        raise SystemExit("Workstation is running. Use TCG or wait for the VMware window to close.")
    args.output.mkdir(mode=0o700, parents=True)
    system, fallback = make_disks(args)
    backup = digest_backup(system)
    results = {"backup_sha256": backup}
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
    assert digest_backup(system) == backup

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

    guest = Guest(args, system, fallback, "dirty-boot")
    try:
        guest.wait_log("USB_SYSTEM_MAINTENANCE stage=needs-filesystem-check error=-117")
        results["dirty_boot"] = guest.attach()
        results["dirty_status"] = guest.status()
        assert results["dirty_status"]["dirty"]
        assert not results["dirty_status"]["window_open"] and not results["dirty_status"]["mounted"]
        assert results["dirty_status"]["writes"] == 0
        log = guest.serial.read_text(errors="replace")
        assert "USB_SYSTEM_READY" not in log and "live-store: ready" not in log
    finally:
        guest.stop()
    assert digest_backup(system) == backup
    # Spoofing the loader's identity through a second attached GPT disk
    # must stop admission even when OVMF can identify its own boot handle.
    guid = subprocess.check_output(["sfdisk", "--part-uuid", system, "1"], text=True).strip()
    run("sfdisk", "--no-reread", "--no-tell-kernel", "--part-uuid", fallback, "1", guid)
    guest = Guest(args, system, fallback, "duplicate-boot-volume")
    try:
        guest.wait_log("USB_SYSTEM_MAINTENANCE stage=boot-volume-ambiguous error=-19")
        guest.attach()
        results["ambiguous_status"] = guest.status()
        assert not results["ambiguous_status"]["mounted"]
        assert not results["ambiguous_status"]["window_open"]
        assert results["ambiguous_status"]["writes"] == 0
    finally:
        guest.stop()
    assert digest_backup(system) == backup
    results["backup_unchanged"] = True
    (args.output / "verdict.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"PASS {args.output / 'verdict.json'}")


if __name__ == "__main__":
    main()

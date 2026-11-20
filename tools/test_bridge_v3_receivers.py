"""Loopback tests of real v3 receivers and disposable filesystems."""
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import uuid

from nacl.signing import SigningKey

from binary_bridge import binary_stream
from bridge_v3 import BULK, STORE, UPDATE, BridgeV3Error, open_bridge_v3
from hardware_bulk_protocol import download, upload as bulk_upload
from hardware_store_protocol import provision
from hardware_update_protocol import INITRD_MAX, KERNEL_MAX, line, promote, record, upload as update_upload

ROOT = Path(__file__).resolve().parents[1]
BOOT = uuid.uuid4().hex


def build(work):
    musl = os.environ.get("MUSL_CC")
    cc = musl or os.environ.get("CC", "cc")
    cxx = (os.environ.get("MUSL_CXX") or musl.removesuffix("gcc") + "g++") if musl else os.environ.get("CXX", "c++")
    link = ["-static"] if musl else []
    vendor = ROOT / "src/third_party/monocypher"
    shared = ROOT / "src/shared/update"
    objects = []
    for source in (shared / "bridge_v3.c", vendor / "monocypher.c", vendor / "monocypher-ed25519.c"):
        obj = work / (source.stem + ".o")
        subprocess.run([cc, "-O2", "-I", vendor, "-c", source, "-o", obj], check=True)
        objects.append(obj)
    definitions = ["-DBROOK_BRIDGE_V3", "-DBROOK_BRIDGE_V3_TEST"]
    programs = {}
    for name, compiler, extra, files in (
        ("update", cc, ["-DBROOK_UPDATE_TEST", "-Wno-unused-result"], ["hardware_update.c"]),
        ("bulk", cc, ["-DBROOK_BULK_TEST"], ["hardware_bulk.c"]),
        ("store", cxx, ["-DBROOK_STORE_RECEIVER_TEST", "-Wno-misleading-indentation"],
         ["hardware_store_receiver.cpp", "hardware_store_transaction.cpp", "hardware_store_validate.cpp"]),
    ):
        programs[name] = work / name
        command = [compiler, "-O2", "-Wall", "-Wextra", "-Werror", *definitions, *extra]
        subprocess.run([*command, *link, *(ROOT / "tools" / f for f in files), *objects, "-o", programs[name]], check=True)
    return programs


def payload_elf():
    raw = bytearray(4096)
    raw[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQ", raw, 16, 2, 62, 1, 0xFFFFFFFF80000000, 64)
    struct.pack_into("<HHH", raw, 52, 64, 56, 1)
    struct.pack_into("<IIQQQQQ", raw, 64, 1, 1, 0, 0xFFFFFFFF80000000, 0, len(raw), len(raw))
    return bytes(raw)


class Receivers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.infrastructure = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.infrastructure.cleanup)
        cls.programs = build(Path(cls.infrastructure.name))

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.signing = SigningKey.generate()
        self.policy = self.directory / "allowed"
        self.policy.write_text(bytes(self.signing.verify_key).hex() + "\n")
        self.policy_hash = hashlib.sha256(self.policy.read_bytes()).hexdigest()
        self.tx = uuid.uuid4().hex

    def receive(self, name, operation, callback, status, *args):
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        prefix = {"update": "BROOK_UPDATE_TEST_", "bulk": "BROOK_BULK_TEST_", "store": "BROOK_STORE_"}[name]
        env = {**os.environ, "BROOK_BRIDGE_V3_TEST_ALLOWLIST": str(self.policy),
               "BROOK_BRIDGE_V3_TEST_POLICY_SHA256": self.policy_hash,
               prefix + "ROOT": str(self.directory), prefix + "KEY": str(self.policy),
               prefix + "LOCK": str(self.directory / "lock"), prefix + "PORT": str(port)}
        command = [self.programs[name], BOOT, self.tx, *args]
        with subprocess.Popen(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
            try:
                self.assertIn("LISTENING", proc.stdout.readline())
                callback(port, operation)
                out, err = proc.communicate(timeout=12)
                self.assertEqual(proc.returncode, status, (out, err))
                return out, err
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)

    def socket(self, port):
        return socket.create_connection(("127.0.0.1", port), timeout=5)

    def setup_update(self):
        kernel = payload_elf()
        initrd = bytes(510) + b"\x55\xaa"
        for slot in "AB":
            (self.directory / slot).mkdir()
            for name, maximum, data in (("KERNEL.ELF", KERNEL_MAX, kernel),
                                        ("INITRD.IMG", INITRD_MAX, initrd)):
                with (self.directory / slot / name).open("wb") as stream:
                    stream.write(data)
                    stream.truncate(maximum)
        (self.directory / "SELECT0.BIN").write_bytes(record(1, 0, kernel, initrd))
        (self.directory / "SELECT1.BIN").write_bytes(bytes(128))
        return kernel, initrd

    def test_update_and_stage_only(self):
        kernel, initrd = self.setup_update()
        def transfer(port, activate):
            result = update_upload("127.0.0.1", port, self.signing, BOOT, self.tx, kernel, initrd, activate=activate)
            self.assertEqual(result["state"], "activated" if activate else "staged")
        self.receive("update", False, transfer, 22)
        self.assertEqual((self.directory / "SELECT1.BIN").read_bytes(), bytes(128))
        self.receive("update", True, transfer, 0)
        self.assertEqual((self.directory / "SELECT1.BIN").read_bytes(), record(2, 1, kernel, initrd))

    def test_promote_has_a_separate_authenticated_operation(self):
        kernel, initrd = self.setup_update()
        (self.directory / "SELECT1.BIN").write_bytes(record(2, 1, kernel, initrd))
        def older(port, _operation):
            result = promote("127.0.0.1", port, self.signing, BOOT, self.tx,
                             0, kernel, initrd, activate=True)
            self.assertEqual((result["state"], result["generation"]), ("promoted", 3))
        self.receive("update", 2, older, 0)
        self.assertEqual((self.directory / "SELECT0.BIN").read_bytes(), record(1, 0, kernel, initrd))
        self.assertEqual((self.directory / "SELECT1.BIN").read_bytes(), record(3, 0, kernel, initrd))

    def test_update_requires_end_before_any_write(self):
        kernel, initrd = self.setup_update()
        changed = b"new" + initrd[3:]
        def interrupted(port, operation):
            with self.socket(port) as sock:
                conn, reserved = binary_stream(sock, self.signing, UPDATE, operation, BOOT, self.tx)
                conn.sendall(b"BRUPLOAD" + reserved + BOOT.encode() + self.tx.encode())
                plan = line(conn)
                conn.sendall(record(plan["generation"], plan["slot"], kernel, changed) + kernel + changed)
                self.assertEqual(line(conn)["state"], "staged")
                conn.sendall(b"ACTIVATE")
        self.receive("update", 1, interrupted, 22)
        self.assertEqual((self.directory / "SELECT1.BIN").read_bytes(), bytes(128))
        with (self.directory / "B/INITRD.IMG").open("rb") as content:
            self.assertEqual(content.read(len(initrd)), initrd)

    def test_bulk_round_trip_and_stage_only(self):
        source = self.directory / "source"
        content = os.urandom(163840)
        source.write_bytes(content)
        def put(port, activate):
            result = bulk_upload("127.0.0.1", port, self.signing, BOOT, self.tx, source, finalize=activate)
            self.assertEqual(result["state"], "completed" if activate else "staged")
        self.receive("bulk", False, put, 19, "put")
        self.assertFalse(list(self.directory.glob("*.part")))
        self.assertFalse((self.directory / f"{self.tx}.bin").exists())
        self.receive("bulk", True, put, 0, "put")
        artifact, target = self.tx, self.directory / "result"
        self.tx = uuid.uuid4().hex
        def get(port, _operation):
            download("127.0.0.1", port, self.signing, BOOT, self.tx, target)
        self.receive("bulk", 2, get, 0, "get", artifact)
        self.assertEqual(target.read_bytes(), content)

    def test_bulk_refuses_legacy_and_wrong_operator(self):
        def legacy(port, _operation):
            with self.socket(port) as sock:
                sock.sendall(b"BRBULK01" + b"0" * 32 + BOOT.encode() + self.tx.encode())
                sock.shutdown(socket.SHUT_WR)
                self.assertEqual(sock.recv(1), b"")
        out, err = self.receive("bulk", 1, legacy, 10, "put")
        self.assertIn("HWBULK_AUTH_REFUSED", err)

        def outsider(port, operation):
            with self.socket(port) as sock, self.assertRaises(BridgeV3Error):
                open_bridge_v3(sock, SigningKey.generate(), BULK, operation,
                               bytes.fromhex(BOOT), bytes.fromhex(self.tx))
        self.receive("bulk", 1, outsider, 10, "put")
        self.assertFalse(list(self.directory.glob("*.part")))

    def test_bulk_rejects_a_different_job_operation(self):
        def request(port,operation):
            with self.socket(port) as sock,self.assertRaises(BridgeV3Error):
                open_bridge_v3(sock,self.signing,BULK,operation,bytes.fromhex(BOOT),bytes.fromhex(self.tx))
        _, err = self.receive("bulk",2,request,10,"put")
        self.assertIn("HWBULK_AUTH_REFUSED",err)
        self.assertFalse(list(self.directory.glob("*.part")))

    def test_changed_public_policy_is_refused(self):
        self.policy.write_text(bytes(self.signing.verify_key).hex() + "\n" +
                               bytes(SigningKey.generate().verify_key).hex() + "\n")
        def request(port,operation):
            with self.socket(port) as sock,self.assertRaises(BridgeV3Error):
                open_bridge_v3(sock,self.signing,BULK,operation,bytes.fromhex(BOOT),bytes.fromhex(self.tx))
        _, err = self.receive("bulk",1,request,10,"put")
        self.assertIn("BRIDGE_V3_POLICY_REFUSED",err)

    def store_input(self):
        size = 16 * 1024 * 1024
        identity = str(uuid.uuid4())
        image = self.directory / "source.img"
        with image.open("wb") as stream:
            stream.truncate(size)
        subprocess.run(["mke2fs", "-F", "-q", "-t", "ext2", "-b", "4096", "-I", "128", "-m", "0",
                        "-O", "none,filetype,sparse_super,large_file", "-U", identity, image], check=True)
        content = image.read_bytes()
        return image, content, f"BRKSTORE1 {size} {identity}".encode()

    def test_store_does_not_publish_without_end(self):
        image, data, manifest = self.store_input()
        digest = hashlib.sha256(data)
        def interrupted(port, operation):
            with self.socket(port) as sock:
                stream, reserved = binary_stream(sock, self.signing, STORE, operation, BOOT, self.tx)
                stream.sendall(b"BRSTORE1" + reserved + BOOT.encode() + self.tx.encode())
                self.assertEqual(line(stream)["state"], "ready")
                stream.sendall(struct.pack("<I", len(manifest)) + digest.digest() + manifest + b"BEGIN001")
                self.assertEqual(line(stream)["state"], "receiving")
                stream.sendall(data)
                self.assertEqual(line(stream)["state"], "staged")
                self.assertTrue((self.directory / "STORE.TMP").exists())
                self.assertFalse((self.directory / "STORE.IMG").exists())
                stream.sendall(b"FINALIZE")
        self.receive("store", 1, interrupted, 21)
        for name in ("STORE.IMG", "STORE.OK", "STORE.TMP", "lock"):
            self.assertFalse((self.directory / name).exists(), name)
        def complete(port, _operation):
            result = provision("127.0.0.1", port, self.signing, BOOT, self.tx, image, manifest, digest.hexdigest())
            self.assertTrue(result["durable"])
        self.receive("store", 1, complete, 0)
        self.assertEqual((self.directory / "STORE.IMG").read_bytes(), data)
        self.assertEqual((self.directory / "STORE.OK").read_bytes(), manifest)


if __name__ == "__main__":
    unittest.main()

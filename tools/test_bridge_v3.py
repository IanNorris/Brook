"""Native Monocypher and host PyNaCl interoperability for bridge v3.

Run with the path to the test-only shared library built from bridge_v3.c.
"""
import ctypes as c
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

from nacl.bindings import crypto_scalarmult, crypto_scalarmult_base
from nacl.signing import SigningKey

import bridge_v3 as wire

NATIVE = c.CDLL(sys.argv.pop(1))
for function in (
    "BrookBridgeV3Verify", "BrookBridgeV3Start", "BrookBridgeV3Seal",
    "BrookBridgeV3Open", "BrookBridgeV3DecodeClient", "BrookBridgeV3DecodeServer",
):
    getattr(NATIVE, function).restype = c.c_int


class NativeStream(c.Structure):
    _fields_ = [
        ("key", c.c_ubyte * 32), ("nonce_prefix", c.c_ubyte * 4),
        ("transcript", c.c_ubyte * 32), ("sequence", c.c_uint64),
        ("traffic", c.c_uint64), ("direction", c.c_uint8),
    ]


def native_seal(stream, kind, plain):
    header = c.create_string_buffer(wire.HEADER.size)
    ciphertext = c.create_string_buffer(len(plain))
    tag = c.create_string_buffer(wire.TAG_BYTES)
    assert NATIVE.BrookBridgeV3Seal(c.byref(stream), header, ciphertext, tag, kind, plain, len(plain))
    return header.raw, ciphertext.raw + tag.raw


def read_bytes(sock, count):
    sock.settimeout(2)
    data = bytearray()
    while len(data) < count:
        fragment = sock.recv(count - len(data))
        if not fragment:
            raise EOFError
        data.extend(fragment)
    return bytes(data)


class BridgeV3(unittest.TestCase):
    boot = b"i" * 16
    transaction = b"t" * 16
    operator = SigningKey(bytes(range(32)))
    server_secret = bytes(range(32, 64))
    client_secret = bytes(range(64, 96))
    server_public = crypto_scalarmult_base(server_secret)
    client_public = crypto_scalarmult_base(client_secret)
    server_hello = wire.SERVER.pack(wire.SERVER_MAGIC, 1, boot, b"s" * 32, server_public)

    def client_hello(self, protocol=wire.UPDATE, operation=1):
        return wire._client_hello(protocol, operation, self.boot, self.transaction,
                                  bytes(self.operator.verify_key), b"c" * 32, self.client_public)

    def native_handshake(self, client, server, proof):
        public = wire.CLIENT.unpack(client)[-1]
        self.assertEqual(NATIVE.BrookBridgeV3Verify(proof, bytes(self.operator.verify_key), client, server), 1)
        rx, tx = NativeStream(), NativeStream()
        self.assertEqual(NATIVE.BrookBridgeV3Start(c.byref(rx), c.byref(tx), self.server_secret,
                                                  public, client, server, proof), 1)
        return rx, tx

    def test_known_answers(self):
        # RFC 8032 test 1, and RFC 7748 section 6.1.
        seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
        public = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        sig = bytes.fromhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
        self.assertEqual(SigningKey(seed).sign(b"").signature, sig)
        self.assertEqual(NATIVE.crypto_ed25519_check(sig, public, b"", 0), 0)
        priv = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        peer = bytes.fromhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
        expected = bytes.fromhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
        out = c.create_string_buffer(32)
        NATIVE.crypto_x25519(out, priv, peer)
        self.assertEqual(out.raw, expected)
        self.assertEqual(crypto_scalarmult(priv, peer), expected)

    def test_canonical_hello_and_transcript(self):
        client = self.client_hello()
        self.assertEqual(NATIVE.BrookBridgeV3DecodeClient(c.create_string_buffer(144), client), 1)
        self.assertEqual(NATIVE.BrookBridgeV3DecodeServer(c.create_string_buffer(80), self.server_hello), 1)
        self.assertEqual(NATIVE.BrookBridgeV3DecodeClient(c.create_string_buffer(144), client[:9] + b"\x02" + client[10:]), 0)
        self.assertEqual(NATIVE.BrookBridgeV3DecodeClient(c.create_string_buffer(144), client[:16] + bytes(16) + client[32:]), 0)
        proof = self.operator.sign(wire.PROOF_LABEL + client + self.server_hello).signature
        expanded = bytes(self.operator) + bytes(self.operator.verify_key)
        native_proof = c.create_string_buffer(64)
        NATIVE.BrookBridgeV3Sign(native_proof, expanded, client, self.server_hello)
        self.assertEqual(native_proof.raw, proof)
        for index in (9, 11, 12, 16, 32, 48, 80, 112):
            changed = bytearray(client)
            changed[index] ^= 1
            self.assertEqual(NATIVE.BrookBridgeV3Verify(proof, bytes(self.operator.verify_key), bytes(changed), self.server_hello), 0)
        self.assertEqual(NATIVE.BrookBridgeV3Verify(proof, bytes(self.operator.verify_key), client, self.server_hello[:-1] + b"z"), 0)
        self.assertEqual(NATIVE.BrookBridgeV3Start(
            c.byref(NativeStream()), c.byref(NativeStream()), self.server_secret,
            b"\x01" + bytes(31), client, self.server_hello, proof), 0)

    def test_record_interop_and_reject(self):
        client = self.client_hello()
        proof = self.operator.sign(wire.PROOF_LABEL + client + self.server_hello).signature
        native_rx, native_tx = self.native_handshake(client, self.server_hello, proof)
        shared = crypto_scalarmult(self.client_secret, self.server_public)
        host_tx, host_rx = wire._streams(shared, client, self.server_hello, proof)
        self.assertEqual(bytes(native_rx.key), host_tx.key)
        self.assertEqual(bytes(native_tx.nonce_prefix), host_rx.nonce_prefix)
        self.assertEqual(host_rx.open(*native_seal(native_tx, wire.READY, b"")), (wire.READY, b""))
        payload = b"maintenance file content" * 1024
        frame = host_tx.seal(wire.DATA, payload)
        self.assertNotIn(payload[:64], frame)
        header, body = frame[:16], frame[16:]
        decoded = c.create_string_buffer(len(payload))
        self.assertEqual(NATIVE.BrookBridgeV3Open(c.byref(native_rx), decoded, header, body[:-16],
                                                 len(payload), body[-16:-1] + bytes([body[-1] ^ 1])), 0)
        self.assertEqual(native_rx.sequence, 0)
        self.assertEqual(NATIVE.BrookBridgeV3Open(c.byref(native_rx), decoded, header, body[:-16],
                                                 len(payload), body[-16:]), 1)
        self.assertEqual(decoded.raw, payload)
        self.assertEqual(NATIVE.BrookBridgeV3Open(c.byref(native_rx), decoded, header, body[:-16],
                                                 len(payload), body[-16:]), 0)
        self.assertEqual(host_rx.open(*native_seal(native_tx, wire.DATA, payload)), (wire.DATA, payload))
        header, body = native_seal(native_tx, wire.END, b"")
        with self.assertRaises(wire.BridgeV3Error):
            host_rx.open(header, body[:-1])
        self.assertEqual(host_rx.open(header, body), (wire.END, b""))

    def test_live_socket_and_checked_terminal_record(self):
        errors = []
        client, server = socket.socketpair()
        with client, server:
            def server_loop():
                try:
                    ch = read_bytes(server, 144)
                    server.sendall(self.server_hello[:20])
                    server.sendall(self.server_hello[20:])
                    proof = read_bytes(server, 64)
                    rx, tx = self.native_handshake(ch, self.server_hello, proof)
                    server.sendall(b"".join(native_seal(tx, wire.READY, b"")))
                    for expected in (b"fixture", b""):
                        header = read_bytes(server, 16)
                        received = read_bytes(server, len(expected))
                        tag = read_bytes(server, 16)
                        out = c.create_string_buffer(len(expected))
                        self.assertEqual(NATIVE.BrookBridgeV3Open(c.byref(rx), out, header, received, len(expected), tag), 1)
                        self.assertEqual(out.raw, expected)
                    server.sendall(b"".join(native_seal(tx, wire.DATA, b"received")))
                    server.sendall(b"".join(native_seal(tx, wire.END, b"")))
                except Exception as exc:
                    errors.append(exc)
                    server.shutdown(socket.SHUT_RDWR)
            thread = threading.Thread(target=server_loop)
            thread.start()
            try:
                connection = wire.open_bridge_v3(client, self.operator, wire.UPDATE, 1, self.boot, self.transaction)
                deadline = time.monotonic() + 2
                connection.send(wire.DATA, b"fixture", deadline)
                connection.send(wire.END, b"", deadline)
                self.assertEqual(connection.receive(deadline), (wire.DATA, b"received"))
                self.assertEqual(connection.receive(deadline), (wire.END, b""))
                with self.assertRaises(wire.BridgeV3Error):
                    connection.receive(deadline)
            finally:
                thread.join(3)
            self.assertEqual(errors, [])
            self.assertFalse(thread.is_alive())

    def test_private_file(self):
        with tempfile.TemporaryDirectory() as directory:
            key = Path(directory) / "operator"
            public = wire.create_operator_key(key)
            self.assertEqual(public, bytes(wire.read_operator_key(key).verify_key))
            with self.assertRaises(FileExistsError):
                wire.create_operator_key(key)
            symlink = key.with_suffix(".link")
            symlink.symlink_to(key)
            with self.assertRaises(OSError):
                wire.read_operator_key(symlink)
            key.chmod(0o640)
            with self.assertRaises(ValueError):
                wire.read_operator_key(key)

    def test_host_refuses_wrong_boot(self):
        peer = wire.SERVER.pack(wire.SERVER_MAGIC, 1, b"?" * 16, b"s" * 32, self.server_public)
        client, server = socket.socketpair()
        with client, server:
            server.sendall(peer)
            with self.assertRaisesRegex(wire.BridgeV3Error, "different boot"):
                wire.open_bridge_v3(client, self.operator, wire.UPDATE, 1, self.boot, self.transaction)
            self.assertEqual(len(read_bytes(server, 144)), 144)
            server.setblocking(False)
            with self.assertRaises(BlockingIOError):
                server.recv(64)


if __name__ == "__main__":
    unittest.main()

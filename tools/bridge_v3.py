"""Brook maintenance transport v3, phase 1.

Only the operator is authenticated. The server hello proves no device identity.
"""
from __future__ import annotations

import hashlib
import hmac
import os
import secrets
import socket
import stat
import struct
import time
from dataclasses import dataclass
from pathlib import Path

from nacl.bindings import (
    crypto_aead_chacha20poly1305_ietf_decrypt,
    crypto_aead_chacha20poly1305_ietf_encrypt,
    crypto_scalarmult,
    crypto_scalarmult_base,
)
from nacl.exceptions import CryptoError
from nacl.signing import SigningKey

COMMAND, UPDATE, BULK, STORE = range(1, 5)
READY, DATA, END = range(1, 4)
CLIENT_MAGIC = b"BRKAUTH3"
SERVER_MAGIC = b"BRKSRV3\0"
SERVER_REFUSAL = b"BRKREFU3"
VERSION = 3
OPERATOR_SUITE = 1
DEVICE_IDENTITY_SUITE = 2
PROOF_LABEL = b"Brook bridge v3 operator proof"
CLIENT_KEY_LABEL = b"Brook bridge v3 client to server"
SERVER_KEY_LABEL = b"Brook bridge v3 server to client"
ZERO_ID = bytes(16)
MAX_RECORD = 65536
MAX_RECORDS = 1 << 28
MAX_TRAFFIC = 16 << 40
TAG_BYTES = 16
HEADER = struct.Struct("<B3xIQ")
CLIENT = struct.Struct("<8sBBBBI16s16s32s32s32s")
SERVER = struct.Struct("<8sQ16s32s32s")


class BridgeV3Error(ConnectionError):
    """An invalid or unexpectedly terminated v3 transport."""


class BridgeV3NoChallenge(BridgeV3Error):
    """A connected peer failed to send any server challenge."""


class BridgeV3Refused(BridgeV3Error):
    """The device explicitly refused the pre-challenge handshake."""


class BridgeV3BootMismatch(BridgeV3Error):
    """The peer advertised a different Brook boot."""


def read_operator_key(path: Path) -> SigningKey:
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or stat.S_IMODE(info.st_mode) != 0o600
        ):
            raise ValueError("operator key must be an owned regular file with mode 0600")
        seed = stream.read(33)
    if len(seed) != 32:
        raise ValueError("operator key must contain exactly one 32-byte Ed25519 seed")
    return SigningKey(seed)


def create_operator_key(path: Path) -> bytes:
    """Create a fresh operator key without replacing an existing file."""
    seed = secrets.token_bytes(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    with os.fdopen(fd, "wb") as stream:
        os.fchmod(stream.fileno(), 0o600)
        stream.write(seed)
        stream.flush()
        os.fsync(stream.fileno())
    return bytes(SigningKey(seed).verify_key)


def _client_hello(protocol: int, operation: int, boot: bytes, transaction: bytes,
                  public_key: bytes, nonce: bytes, ephemeral: bytes) -> bytes:
    if len(boot) != 16 or len(transaction) != 16:
        raise ValueError("boot and transaction IDs must be 16 bytes")
    is_status = protocol == COMMAND and operation == 0
    if (
        operation not in {COMMAND: (0, 1), UPDATE: (1, 2, 3), BULK: (1, 2, 3, 4, 5, 6), STORE: (1,)}.get(protocol, ())
        or (not is_status and (boot == ZERO_ID or transaction == ZERO_ID))
        or len(public_key) != 32 or len(nonce) != 32 or len(ephemeral) != 32
        or not any(nonce) or not any(ephemeral)
    ):
        raise ValueError("invalid v3 hello")
    return CLIENT.pack(
        CLIENT_MAGIC, VERSION, OPERATOR_SUITE, protocol, operation, 1233 + protocol,
        boot, transaction, hashlib.sha256(public_key).digest(), nonce, ephemeral,
    )


def _server_hello(wire: bytes, expected_boot: bytes) -> tuple[bytes, bytes]:
    try:
        magic, suite, boot, nonce, ephemeral = SERVER.unpack(wire)
    except struct.error as exc:
        raise BridgeV3Error("invalid v3 server challenge size") from exc
    if magic != SERVER_MAGIC or suite != OPERATOR_SUITE or not all(map(any, (boot, nonce, ephemeral))):
        raise BridgeV3Error("invalid v3 server challenge")
    if expected_boot != ZERO_ID and boot != expected_boot:
        raise BridgeV3BootMismatch("v3 server challenge names a different boot")
    return boot, ephemeral


def _kdf(shared: bytes, transcript: bytes, label: bytes) -> tuple[bytes, bytes]:
    prk = hmac.digest(transcript, shared, "sha256")
    first = hmac.digest(prk, label + b"\x01", "sha256")
    second = hmac.digest(prk, first + label + b"\x02", "sha256")
    return first, second[:4]


@dataclass
class RecordStream:
    key: bytes
    nonce_prefix: bytes
    transcript: bytes
    direction: int
    sequence: int = 0
    traffic: int = 0

    def _valid(self, kind: int, length: int) -> bool:
        if self.sequence >= MAX_RECORDS or not 0 <= length <= MAX_RECORD or self.traffic > MAX_TRAFFIC - length:
            return False
        if kind == READY:
            return self.direction == 2 and self.sequence == 0 and length == 0
        if kind == END:
            return length == 0
        return kind == DATA and length > 0

    def _nonce(self) -> bytes:
        return self.nonce_prefix + struct.pack("<Q", self.sequence)

    def _aad(self, header: bytes) -> bytes:
        return self.transcript + bytes([self.direction]) + header

    def inspect(self, header: bytes) -> tuple[int, int]:
        kind, length, sequence = HEADER.unpack(header)
        if any(header[1:4]) or sequence != self.sequence or not self._valid(kind, length):
            raise BridgeV3Error("invalid v3 record header")
        return kind, length

    def seal(self, kind: int, plain: bytes) -> bytes:
        length = len(plain)
        if not self._valid(kind, length):
            raise BridgeV3Error("invalid v3 outbound record")
        header = HEADER.pack(kind, length, self.sequence)
        body = crypto_aead_chacha20poly1305_ietf_encrypt(plain, self._aad(header), self._nonce(), self.key)
        self.sequence += 1
        self.traffic += length
        return header + body

    def open(self, header: bytes, body: bytes) -> tuple[int, bytes]:
        kind, length = self.inspect(header)
        if len(body) != length + TAG_BYTES:
            raise BridgeV3Error("invalid v3 record body size")
        try:
            plain = crypto_aead_chacha20poly1305_ietf_decrypt(body, self._aad(header), self._nonce(), self.key)
        except CryptoError as exc:
            raise BridgeV3Error("invalid v3 record tag") from exc
        self.sequence += 1
        self.traffic += length
        return kind, plain


def _streams(shared: bytes, client: bytes, server: bytes, proof: bytes) -> tuple[RecordStream, RecordStream]:
    transcript = hashlib.sha256(client + server + proof).digest()
    out_key, out_nonce = _kdf(shared, transcript, CLIENT_KEY_LABEL)
    in_key, in_nonce = _kdf(shared, transcript, SERVER_KEY_LABEL)
    return RecordStream(out_key, out_nonce, transcript, 1), RecordStream(in_key, in_nonce, transcript, 2)


def _remaining(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("v3 transport deadline expired")
    return remaining


def _receive(sock: socket.socket, count: int, deadline: float, *, challenge: bool = False) -> bytes:
    data = bytearray()
    while len(data) < count:
        try:
            sock.settimeout(_remaining(deadline))
            fragment = sock.recv(count - len(data))
        except TimeoutError as exc:
            if challenge:
                if not data:
                    raise BridgeV3NoChallenge("v3 server challenge timed out") from exc
                raise BridgeV3Error("incomplete v3 server challenge") from exc
            raise
        if not fragment:
            if challenge and not data:
                raise BridgeV3NoChallenge("peer closed before the v3 server challenge")
            raise BridgeV3Error("v3 stream ended without the expected bytes")
        data.extend(fragment)
    return bytes(data)


@dataclass
class BridgeV3Connection:
    socket: socket.socket
    boot_id: bytes
    send_stream: RecordStream
    receive_stream: RecordStream
    sent_end: bool = False
    received_end: bool = False

    def send(self, kind: int, plain: bytes, deadline: float) -> None:
        if self.sent_end or kind == READY:
            raise BridgeV3Error("v3 request stream already closed or wrong direction")
        frame = self.send_stream.seal(kind, plain)
        self.socket.settimeout(_remaining(deadline))
        self.socket.sendall(frame)
        self.sent_end = kind == END

    def receive(self, deadline: float) -> tuple[int, bytes]:
        if self.received_end:
            raise BridgeV3Error("v3 response stream already closed")
        kind, plain = _receive_record(self.socket, self.receive_stream, deadline)
        if kind == READY:
            raise BridgeV3Error("unexpected v3 ready record")
        self.received_end = kind == END
        return kind, plain


def _receive_record(sock: socket.socket, stream: RecordStream, deadline: float) -> tuple[int, bytes]:
    header = _receive(sock, HEADER.size, deadline)
    _, length = stream.inspect(header)
    return stream.open(header, _receive(sock, length + TAG_BYTES, deadline))


def open_bridge_v3(sock: socket.socket, key: SigningKey, protocol: int, operation: int,
                   boot_id: bytes, transaction: bytes, *, deadline: float | None = None) -> BridgeV3Connection:
    """Authenticate the operator on a connected socket and require encrypted ready."""
    deadline = time.monotonic() + 4 if deadline is None else deadline
    private = secrets.token_bytes(32)
    client = _client_hello(protocol, operation, boot_id, transaction, bytes(key.verify_key),
                           secrets.token_bytes(32), crypto_scalarmult_base(private))
    sock.settimeout(_remaining(deadline))
    sock.sendall(client)
    prefix = _receive(sock, len(SERVER_MAGIC), deadline, challenge=True)
    if prefix == SERVER_REFUSAL:
        raise BridgeV3Refused("v3 operator admission refused")
    if prefix != SERVER_MAGIC:
        raise BridgeV3Error("unexpected v3 server challenge preface")
    try:
        server = prefix + _receive(sock, SERVER.size - len(prefix), deadline)
    except (TimeoutError, BridgeV3Error) as exc:
        raise BridgeV3Error("incomplete v3 server challenge") from exc
    actual_boot, peer_ephemeral = _server_hello(server, boot_id)
    try:
        shared = crypto_scalarmult(private, peer_ephemeral)
    except CryptoError as exc:
        raise BridgeV3Error("invalid v3 peer X25519 key") from exc
    proof = key.sign(PROOF_LABEL + client + server).signature
    tx, rx = _streams(shared, client, server, proof)
    sock.settimeout(_remaining(deadline))
    sock.sendall(proof)
    kind, _ = _receive_record(sock, rx, deadline)
    if kind != READY:
        raise BridgeV3Error("missing v3 encrypted ready")
    return BridgeV3Connection(sock, actual_boot, tx, rx)

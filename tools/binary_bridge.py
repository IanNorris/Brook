"""Select one bridge transport before sending a binary receiver request."""

import time

BULK_OPERATIONS = {"put": 1, "get": 2, "getz": 3, "frame": 4, "framez": 5, "disk": 6}


class VerifiedByteStream:
    """Preserve existing protocol message framing inside encrypted records."""

    def __init__(self, connection):
        self.transport = connection
        self.pending = memoryview(b"")

    def settimeout(self, timeout):
        self.transport.socket.settimeout(timeout)
        self.timeout = timeout

    def sendall(self, bytes_to_send):
        from bridge_v3 import DATA, MAX_RECORD

        deadline = time.monotonic() + self.timeout
        data = memoryview(bytes_to_send)
        for first in range(0, len(data), MAX_RECORD):
            self.transport.send(DATA, bytes(data[first:first + MAX_RECORD]), deadline)

    def recv(self, length):
        from bridge_v3 import DATA

        if not self.pending and not self.transport.received_end:
            kind, data = self.transport.receive(time.monotonic() + self.timeout)
            if kind == DATA:
                self.pending = memoryview(data)
        result = self.pending[:length].tobytes()
        self.pending = self.pending[length:]
        return result

    def finish_request(self):
        from bridge_v3 import END

        self.transport.send(END, b"", time.monotonic() + self.timeout)

    def require_response_end(self):
        from bridge_v3 import BridgeV3Error, END

        if self.pending:
            raise BridgeV3Error("unexpected extra bytes before v3 response end")
        if not self.transport.received_end:
            kind, _ = self.transport.receive(time.monotonic() + self.timeout)
            if kind != END:
                raise BridgeV3Error("v3 response missing authenticated end")

    def close(self):
        self.transport.socket.close()


def binary_stream(sock, key, protocol, operation, boot, transaction):
    """Return the byte stream and its legacy handoff field.

    A string is an already validated legacy credential. A SigningKey is v3.
    No handshake failure changes this selection.
    """
    if isinstance(key, str):
        if len(key) != 32 or any(c not in '0123456789abcdef' for c in key):
            raise ValueError('invalid legacy bridge credential')
        return sock, key.encode()

    from bridge_v3 import open_bridge_v3

    timeout = sock.gettimeout()
    authenticated = open_bridge_v3(sock, key, protocol, operation, bytes.fromhex(boot), bytes.fromhex(transaction))
    stream = VerifiedByteStream(authenticated)
    stream.settimeout(timeout if timeout is not None else 180)
    return stream, bytes(32)


def finish_request(stream):
    if isinstance(stream, VerifiedByteStream):
        stream.finish_request()


def require_response_end(stream):
    if isinstance(stream, VerifiedByteStream):
        stream.require_response_end()


def session_auth_mode(session):
    """Require an explicitly attached transport before opening any connection."""
    mode = session.get("auth_mode")
    if mode not in ("legacy", "v3"):
        raise ValueError("Missing or invalid hardware authentication mode; reattach explicitly")
    if mode == "v3":
        operator_key = session.get("operator_key_file")
        if not isinstance(operator_key, str) or not operator_key:
            raise ValueError("Missing hardware operator key path; reattach explicitly")
    return mode


def receiver_key(session, legacy_key_file):
    """Use the same selected authentication for commands and data transfers."""
    from pathlib import Path

    if session_auth_mode(session) == "v3":
        from bridge_v3 import read_operator_key

        return read_operator_key(Path(session["operator_key_file"]))
    if legacy_key_file is None:
        raise ValueError("An explicit legacy hardware session requires a legacy key file")
    with Path(legacy_key_file).open() as stream:
        text = stream.read(33)
    if len(text) != 32 or any(c not in "0123456789abcdef" for c in text):
        raise ValueError("expected a 32-character legacy credential without a newline")
    return text

"""Minimal QEMU Human Monitor Protocol (HMP) client over a UNIX socket.

Used by the P2 gate to inject input (sendkey), capture frames (screendump),
and shut the VM down cleanly (quit). Kept dependency-free so it runs in the
bare in-container shell.
"""
import socket
import time


class Hmp:
    def __init__(self, sock_path: str, connect_timeout: float = 30.0):
        self.sock_path = sock_path
        self.connect_timeout = connect_timeout
        self._s = None

    def connect(self):
        deadline = time.time() + self.connect_timeout
        last_err = None
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5.0)
                s.connect(self.sock_path)
                self._s = s
                time.sleep(0.2)
                self._drain()
                return self
            except OSError as e:
                last_err = e
                time.sleep(0.3)
        raise TimeoutError(f"HMP connect to {self.sock_path} failed: {last_err}")

    def _drain(self):
        try:
            self._s.setblocking(False)
            while True:
                if not self._s.recv(65536):
                    break
        except (BlockingIOError, OSError):
            pass
        finally:
            if self._s:
                self._s.setblocking(True)
                self._s.settimeout(5.0)

    def cmd(self, text: str, settle: float = 0.15) -> str:
        """Send one HMP command and return whatever the monitor echoes back."""
        if self._s is None:
            self.connect()
        self._s.sendall((text + "\n").encode())
        time.sleep(settle)
        out = b""
        try:
            self._s.setblocking(False)
            for _ in range(20):
                try:
                    chunk = self._s.recv(65536)
                    if not chunk:
                        break
                    out += chunk
                except BlockingIOError:
                    time.sleep(0.05)
        finally:
            self._s.setblocking(True)
            self._s.settimeout(5.0)
        return out.decode(errors="replace")

    def sendkey(self, keyspec: str, hold_ms: int = 0):
        # keyspec is QEMU key notation, e.g. "ctrl-a", "shift-b", "1".
        if hold_ms:
            self.cmd(f"sendkey {keyspec} {hold_ms}")
        else:
            self.cmd(f"sendkey {keyspec}")

    def screendump(self, path: str):
        return self.cmd(f"screendump {path}", settle=0.5)

    def quit(self):
        try:
            self.cmd("quit", settle=0.1)
        except OSError:
            pass

    def close(self):
        if self._s is not None:
            try:
                self._s.close()
            finally:
                self._s = None

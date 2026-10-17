"""VM identity, scoped artifacts and QMP transport (stdlib only)."""
import contextlib
import array
import fcntl
import json
import os
from pathlib import Path
import socket
import time
import uuid

ROOT = Path(os.environ.get("BROOK_ROOT", Path(__file__).resolve().parents[2])).resolve()
REGISTRY = Path(os.environ.get("BROOK_VM_REGISTRY", ROOT / "build/vm-bridge")).resolve()


def process_identity(pid):
    text = Path(f"/proc/{pid}/stat").read_text()
    return text.rsplit(")", 1)[1].split()[19]


def process_is_alive(pid, identity):
    try:
        fields=Path(f"/proc/{pid}/stat").read_text().rsplit(")",1)[1].split()
        return fields[0] not in {"Z","X"} and fields[19]==identity
    except (OSError,IndexError,TypeError):return False


@contextlib.contextmanager
def recording_lock(session):
    load_session(session,live=False)
    with (REGISTRY/session/"video-recording.lock").open("a") as lock:
        deadline=time.monotonic()+10
        while True:
            try:
                fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);break
            except BlockingIOError:
                if time.monotonic()>deadline:raise TimeoutError("VM recording state is busy")
                time.sleep(.01)
        yield


def write_json(path, value):
    path = Path(path)
    temp = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    with temp.open("x") as out:
        os.chmod(temp, 0o600)
        json.dump(value, out)
        out.flush()
        os.fsync(out.fileno())
    temp.replace(path)


def load_session(session, live=True):
    if not isinstance(session, str) or len(session) != 32 or any(c not in "0123456789abcdef" for c in session):
        raise ValueError("Invalid session ID")
    path = REGISTRY / session / "session.json"
    if path.is_symlink() or path.stat().st_uid != os.getuid():
        raise ValueError("Session descriptor is not owned by this user")
    data = json.loads(path.read_text())
    if data["session"] != session or data["owner_uid"] != os.getuid():
        raise ValueError("Session identity mismatch")
    if not live:
        return data
    if data["state"] != "running" or process_identity(data["pid"]) != data["process_identity"]:
        raise ValueError("VM stopped or process identity changed")
    cmd = Path(f"/proc/{data['pid']}/cmdline").read_bytes().split(b"\0")
    if str(REGISTRY / session / "qmp.sock").encode() not in b" ".join(cmd):
        raise ValueError("QEMU endpoint identity mismatch")
    return data


@contextlib.contextmanager
def session_lock(session, timeout=10):
    data = load_session(session)
    with (REGISTRY / session / "operation.lock").open("a") as lock:
        deadline = time.monotonic() + timeout
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise TimeoutError("Another client owns the VM operation lock")
                time.sleep(.05)
        # Revalidate after acquiring: the process may have exited while waiting.
        yield load_session(session)


class QMP:
    def __init__(self, session, timeout=10):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(str(REGISTRY / session / "qmp.sock"))
        self.reader = self.sock.makefile("rb")
        self.sequence = 0
        self.version = self.read()["QMP"]["version"]
        self.call("qmp_capabilities")

    def read(self):
        line = self.reader.readline(2 * 1024 * 1024)
        if not line or not line.endswith(b"\n"):
            raise ConnectionError("QMP closed or oversized message")
        return json.loads(line)

    def call(self, execute, arguments=None, fd=None):
        self.sequence += 1
        request = {"execute": execute, "id": self.sequence}
        if arguments is not None:
            request["arguments"] = arguments
        payload=json.dumps(request).encode()+b"\n"
        if fd is None:
            self.sock.sendall(payload)
        else:
            sent=self.sock.sendmsg([payload],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array("i",[fd]))])
            if sent<len(payload):self.sock.sendall(payload[sent:])
        while True:
            response = self.read()
            if response.get("id") != self.sequence:
                continue
            if "error" in response:
                raise RuntimeError(response["error"].get("desc", str(response["error"])))
            return response["return"]

    def hmp(self, command, cpu=None):
        arguments = {"command-line": command}
        if cpu is not None:
            arguments["cpu-index"] = cpu
        result = self.call("human-monitor-command", arguments)
        if result.strip().startswith(("Error", "unknown command", "invalid parameter")):
            raise RuntimeError(result.strip())
        return result

    def close(self):
        self.reader.close()
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def artifact(session, suffix):
    directory = REGISTRY / session / "artifacts"
    directory.mkdir(exist_ok=True, mode=0o700)
    return directory / (uuid.uuid4().hex + suffix)

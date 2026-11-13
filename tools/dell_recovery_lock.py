"""Serialize Dell maintenance and a recovery cycle on this host."""

from contextlib import contextmanager
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import uuid

ROOT = Path(__file__).resolve().parents[1]
PENDING_CYCLE = ("switching_off", "waiting_off", "restoring", "restore_failed", "verifying")


def state_directory(host, port):
    host = str(ipaddress.IPv4Address(host))
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("Invalid bridge port")
    registry = Path(os.environ.get("BROOK_HARDWARE_REGISTRY", ROOT / "build/hardware-bridge"))
    return registry / "ac-recovery" / f"{host}-{port}"


@contextmanager
def endpoint_lock(directory, name, nonblocking=False):
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(directory / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        mode = fcntl.LOCK_EX | (fcntl.LOCK_NB if nonblocking else 0)
        fcntl.flock(fd, mode)
        yield
    finally:
        os.close(fd)


def maintenance_lock(host, port, nonblocking=False):
    return endpoint_lock(state_directory(host, port), "maintenance.lock", nonblocking)


def maintenance_epoch(directory):
    try:
        return (directory / "maintenance.epoch").read_text()
    except FileNotFoundError:
        return None


@contextmanager
def inhibit_recovery(host, port):
    directory = state_directory(host, port)
    with maintenance_lock(host, port):
        try:
            journal = json.loads((directory / "journal.json").read_text())
        except FileNotFoundError:
            journal = {}
        if journal.get("state") in PENDING_CYCLE:
            raise RuntimeError("Resume the pending Dell recovery before starting maintenance")
        # Invalidate the old arm even if monitoring misses this entire operation
        # or maintenance loses the bridge before a fresh boot can be verified.
        path = directory / "maintenance.epoch"
        fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w") as stream:
            stream.write(uuid.uuid4().hex)
            stream.flush()
            os.fsync(stream.fileno())
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        yield

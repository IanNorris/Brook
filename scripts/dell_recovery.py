#!/usr/bin/env python3
"""Monitor one Brook Dell and optionally recover it through its AC relay."""

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/vm_bridge")]
from dell_recovery_lock import (
    PENDING_CYCLE, endpoint_lock, inhibit_recovery, maintenance_epoch,
    maintenance_lock, state_directory,
)
from guest_transport import exchange
from hardware import Hardware
from probe_hardware_link import probe


POLL_SECONDS = 10
OUTAGE_SECONDS = 60
AC_OFF_SECONDS = 90
VERIFY_SECONDS = 180
RESTORE_SECONDS = 60
BOOT_ID = re.compile(r"[0-9a-f]{32}")
PAYLOAD_ID = re.compile(r"[A-Za-z0-9_.-]{1,95}")


def valid_boot_id(value):
    return isinstance(value, str) and bool(BOOT_ID.fullmatch(value)) and value != "0" * 32


def ready(caps):
    recovery = caps.get("ac_recovery")
    desktop = caps.get("writable_desktop")
    power = caps.get("power")
    return (isinstance(recovery, dict) and recovery.get("api") == 1
            and recovery.get("ready") is True and recovery.get("source") == "online"
            and isinstance(desktop, dict) and desktop.get("ready") is True
            and desktop.get("state_uncertain") is False
            and isinstance(power, dict) and power.get("power_off") is True)


def payload_id(caps):
    value = caps.get("payload_id")
    return value if isinstance(value, str) and PAYLOAD_ID.fullmatch(value) else None


def shutdown_finalization(caps, armed_boot):
    record = caps.get("ac_shutdown_record")
    if (not isinstance(record, dict) or record.get("stage") != "checked_finalization"
            or not valid_boot_id(record.get("boot_id"))
            or type(record.get("sequence")) is not int or record["sequence"] <= 0):
        return {"status": "unavailable"}
    return {"status": "matched" if record["boot_id"] == armed_boot else "different_boot",
            "boot_id": record["boot_id"], "sequence": record["sequence"]}


class Journal:
    def __init__(self, directory):
        self.directory = directory
        self.path = directory / "journal.json"

    def load(self):
        try:
            state = json.loads(self.path.read_text())
        except FileNotFoundError:
            return {"version": 1, "state": "disarmed"}
        if state.get("version") != 1 or state.get("state") not in (
            *PENDING_CYCLE, "disarmed", "armed", "spent"
        ):
            raise ValueError("Unsupported recovery journal")
        return state

    def save(self, state):
        with tempfile.NamedTemporaryFile(mode="w", dir=self.directory, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                json.dump(state, stream)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
                temporary.replace(self.path)
                fd = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(fd)
                finally:
                    os.close(fd)
            finally:
                temporary.unlink(missing_ok=True)


def read_caps(host, port):
    try:
        caps, = exchange(host, port, ["bridge hardware caps"], 5)
        if caps.get("boot_id") == "0" * 32 and caps.get("boot_guard") is True:
            epoch, caps = exchange(host, port, [
                "bridge epoch init " + uuid.uuid4().hex, "bridge hardware caps",
            ], 5)
            if epoch.get("boot_id") != caps.get("boot_id"):
                return None
    except (OSError, ValueError, RuntimeError):
        return None
    if caps.get("hardware_api") != 1 or caps.get("boot_guard") is not True:
        return None
    return caps if valid_boot_id(caps.get("boot_id")) else None


def relay(config, action):
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts/tasmota_power.py"),
         "--config", str(config), action],
        capture_output=True, text=True, timeout=8, check=False,
    )
    if result.returncode == 0 and result.stdout.strip() in ("Power1: ON", "Power1: OFF"):
        return result.stdout.strip().split()[-1]
    return None


class Recovery:
    def __init__(self, journal, host, port, config, *, clock=time.monotonic,
                 sleep=time.sleep, caps=read_caps, switch=relay, diagnose=probe):
        self.journal = journal
        self.host = host
        self.port = port
        self.config = config.resolve()
        self.clock = clock
        self.sleep = sleep
        self.caps = caps
        self.switch = switch
        self.diagnose = diagnose
        self.host_boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        self.state = journal.load()
        self.outage_since = None
        self.diagnosed = False
        if self.state["state"] in (*PENDING_CYCLE, "armed") and self.state.get("relay_config") != str(self.config):
            raise ValueError("Use the original relay configuration for this operation")

    def record(self, phase, **details):
        self.state = {**self.state, **details, "state": phase}
        self.journal.save(self.state)
        print(json.dumps(self.state), flush=True)

    def relay(self, action):
        try:
            return self.switch(self.config, action)
        except (OSError, subprocess.TimeoutExpired):
            return None

    def arm(self):
        if self.state["state"] in PENDING_CYCLE:
            raise RuntimeError("Resume the pending recovery before rearming")
        with maintenance_lock(self.host, self.port, nonblocking=True):
            caps = self.caps(self.host, self.port)
            if caps is None or not ready(caps):
                raise RuntimeError("A reachable boot must advertise ready AC recovery on AC power")
            payload = payload_id(caps)
            if payload is None:
                raise RuntimeError("A reachable boot must identify its recovery payload")
            if self.relay("status") != "ON":
                raise RuntimeError("AC relay must report ON before arming")
            self.state = {"version": 1}
            self.record("armed", boot_id=caps["boot_id"], relay_config=str(self.config),
                        expected_payload_id=payload,
                        maintenance_epoch=maintenance_epoch(self.journal.directory))

    def disarm(self):
        if self.state["state"] in PENDING_CYCLE:
            raise RuntimeError("Resume the pending recovery to restore AC")
        self.state = {"version": 1}
        self.record("disarmed")

    def run(self):
        if self.state["state"] in PENDING_CYCLE:
            with maintenance_lock(self.host, self.port, nonblocking=True):
                return self.finish_cycle_interruptibly()
        if self.state["state"] == "spent":
            return 1
        while True:
            # Maintenance holds this lock across the complete guest transaction.
            try:
                with maintenance_lock(self.host, self.port, nonblocking=True):
                    outcome = self.poll()
                    if outcome is not None:
                        return outcome
            except BlockingIOError:
                self.outage_since = None
                self.diagnosed = False
            self.sleep(POLL_SECONDS)

    def poll(self):
        if (self.state["state"] == "armed"
                and self.state.get("maintenance_epoch") != maintenance_epoch(self.journal.directory)):
            self.record("spent", outcome="maintenance_invalidated_arm")
            return 1
        caps = self.caps(self.host, self.port)
        if caps is not None:
            if self.state["state"] == "armed" and (
                caps["boot_id"] != self.state["boot_id"] or not ready(caps)
                or not payload_id(caps) or payload_id(caps) != self.state.get("expected_payload_id")
            ):
                self.record("spent", outcome="armed_boot_or_readiness_changed",
                            observed_boot_id=caps["boot_id"])
                return 1
            self.outage_since = None
            self.diagnosed = False
            return None

        now = self.clock()
        if self.outage_since is None:
            self.outage_since = now
        if now - self.outage_since < OUTAGE_SECONDS or self.diagnosed:
            return None
        self.diagnosed = True
        diagnostic = self.diagnose(self.host, self.port, timeout=5)
        if diagnostic["bridge"].get("stage") == "capabilities_received":
            self.outage_since = None
            self.diagnosed = False
            return None
        if self.state["state"] != "armed":
            print(json.dumps({"event": "bridge_unreachable", "diagnostic": diagnostic}), flush=True)
            return None
        if self.relay("status") != "ON":
            self.record("spent", outcome="relay_not_confirmed_on", diagnostic=diagnostic)
            return 1

        # Consume the attempt before sending OFF. A restarted process may
        # reconcile the relay but never sends another OFF for this attempt.
        self.record("switching_off", host_boot=self.host_boot, diagnostic=diagnostic)
        try:
            result = self.relay("off")
            if result is None:
                result = self.relay("status")
            if result == "OFF":
                self.record("waiting_off", deadline=self.clock() + AC_OFF_SECONDS)
            else:
                self.record("restoring", off_unconfirmed=True)
            return self.finish_cycle()
        except (KeyboardInterrupt, SystemExit):
            self.restore_on_interruption()
            raise
        except OSError:
            # Journal failure after relay dispatch must not prevent restoring
            # AC. The last durable state can still be reconciled on restart.
            self.relay("on")
            raise

    def finish_cycle_interruptibly(self):
        try:
            return self.finish_cycle()
        except (KeyboardInterrupt, SystemExit):
            if self.state["state"] != "verifying":
                self.restore_on_interruption()
            raise
        except OSError:
            self.relay("on")
            raise

    def restore_on_interruption(self):
        try:
            self.restore_ac()
        except OSError:
            self.relay("on")
            raise

    def restore_ac(self):
        self.record("restoring")
        deadline = self.clock() + RESTORE_SECONDS
        while True:
            if self.relay("on") == "ON" or self.relay("status") == "ON":
                self.record("verifying", host_boot=self.host_boot,
                            deadline=self.clock() + VERIFY_SECONDS)
                return True
            if self.clock() >= deadline:
                self.record("restore_failed", outcome="relay_on_unconfirmed")
                return False
            self.sleep(POLL_SECONDS)

    def finish_cycle(self):
        phase = self.state["state"]
        if phase == "switching_off":
            # OFF may have executed just before interruption. Give an observed
            # OFF relay a full shutdown interval before restoring it.
            if self.relay("status") == "OFF":
                self.record("waiting_off", host_boot=self.host_boot,
                            deadline=self.clock() + AC_OFF_SECONDS)
                phase = "waiting_off"
            else:
                self.record("restoring", off_unconfirmed=True)
        if phase == "waiting_off" and self.state["host_boot"] == self.host_boot:
            self.sleep(max(0, self.state["deadline"] - self.clock()))
        if phase != "verifying" and not self.restore_ac():
            return 1
        deadline = self.state["deadline"] if self.state["host_boot"] == self.host_boot else self.clock() + VERIFY_SECONDS
        last_caps = None
        while self.clock() < deadline:
            caps = self.caps(self.host, self.port)
            if caps is not None:
                last_caps = caps
                payload = payload_id(caps)
                if caps["boot_id"] == self.state["boot_id"]:
                    return self.finish_verification("same_boot_reachable", caps)
                if payload and payload != self.state.get("expected_payload_id"):
                    return self.finish_verification("unexpected_payload", caps)
                if payload and ready(caps):
                    return self.finish_verification("wake_verified", caps)
            self.sleep(POLL_SECONDS)
        if last_caps is not None:
            return self.finish_verification("new_boot_unverified", last_caps)
        self.record("spent", outcome="bridge_still_unreachable")
        return 1

    def finish_verification(self, outcome, caps):
        self.record("spent", outcome=outcome, observed_boot_id=caps["boot_id"],
                    observed_payload_id=payload_id(caps), build_id=caps.get("build_id"),
                    recovery=caps.get("ac_recovery"),
                    shutdown_finalization=shutdown_finalization(caps, self.state["boot_id"]))
        return 0 if outcome == "wake_verified" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Explicit Dell IPv4 address")
    parser.add_argument("--port", default=1234, type=int)
    parser.add_argument("--config", default=Path.home() / ".config/dellpower.txt", type=Path)
    commands = parser.add_subparsers(dest="action", required=True)
    for action in ("arm", "disarm", "status", "run"):
        commands.add_parser(action)
    inhibit = commands.add_parser("inhibit", help="Hold recovery while a maintenance command runs")
    inhibit.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    host, port = Hardware.endpoint(args.host, args.port)
    directory = state_directory(host, port)
    journal = Journal(directory)
    if args.action == "status":
        print(json.dumps(journal.load(), indent=2))
        return 0
    if args.action == "inhibit":
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        if not command:
            parser.error("inhibit requires a command")
        with inhibit_recovery(host, port):
            return subprocess.run(command, check=False).returncode
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    try:
        with endpoint_lock(directory, "controller.lock", nonblocking=True):
            recovery = Recovery(journal, host, port, args.config)
            return getattr(recovery, args.action)()
    except BlockingIOError:
        parser.exit(1, "Dell recovery controller or maintenance already holds the lock\n")
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    sys.exit(main())

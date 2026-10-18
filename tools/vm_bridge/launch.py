#!/usr/bin/env python3
"""Publish an opted-in QEMU run independently of any MCP client."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import threading
import time
import uuid

from common import REGISTRY, QMP, process_identity, write_json
from reset import launch_reboot_action
from lifecycle import launch_args as lifecycle_launch_args


def recording_display_args(command):
    """Initialize private QMP VNC/audio access alongside the selected UI."""
    args=list(command)
    if "-vnc" not in args:args.extend(["-vnc","none"])
    index=args.index("-vnc")+1
    # run-qemu's output backend is hda0, including silent playback. Preserve
    # an explicitly selected VNC audio backend and all display/security flags.
    has_audio=any(args[i]=="-audiodev" and "id=hda0" in args[i+1].split(",")
                  for i in range(len(args)-1))
    if has_audio and "audiodev=" not in args[index]:args[index]+=",audiodev=hda0"
    return args


def supervise(command, kernel, disk, debug_port, provenance, scenario):
    REGISTRY.mkdir(parents=True, exist_ok=True, mode=0o700)
    session = uuid.uuid4().hex
    directory = REGISTRY / session
    directory.mkdir(mode=0o700)
    # Private registry sockets must fit sockaddr_un's 108-byte limit.
    if len(str(directory / "qmp.sock").encode()) >= 100:
        raise ValueError("BROOK_VM_REGISTRY path is too long for a Unix socket")
    args = recording_display_args(command)
    for option, value in [("-monitor", f"unix:{directory}/hmp.sock,server=on,wait=off"),
                          ("-serial", f"file:{directory}/serial.log")]:
        if option in args:
            index = args.index(option)
            args[index + 1] = value
        else:
            args.extend([option, value])
    lifecycle_mode = os.environ.get("BROOK_VM_LIFECYCLE", "1")
    if lifecycle_mode not in {"0", "1"}:
        raise ValueError("BROOK_VM_LIFECYCLE must be 0 or 1")
    lifecycle_enabled = lifecycle_mode == "1"
    args = lifecycle_launch_args(args, session, directory, enabled=lifecycle_enabled)
    args.extend(["-qmp", f"unix:{directory}/qmp.sock,server=on,wait=off"])
    kernel_copy = directory / "kernel.elf"
    shutil.copy2(kernel, kernel_copy)
    # The supervisor owns logging, so a client disconnect cannot lose logs.
    log = (directory / "qemu.log").open("wb")
    child = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    descriptor = {"schema_version": 1, "session": session, "generation": session,
                  "pid": child.pid, "process_identity": process_identity(child.pid),
                  "owner_uid": os.getuid(), "state": "running", "created_at": time.time(),
                  "provenance": provenance, "scenario": scenario,
                  "lifecycle_channel": {"api": 1 if lifecycle_enabled else 0,
                                        "socket": str(directory / "lifecycle.sock") if lifecycle_enabled else None,
                                        "commands": ["hello", "sync", "status"] if lifecycle_enabled else []},
                  "qmp": str(directory / "qmp.sock"), "run_directory": str(directory),
                  "serial_log": str(directory / "serial.log"), "qemu_log": str(directory / "qemu.log"),
                  "boot_disk": str(Path(disk).resolve()), "guest_debug_port": debug_port,
                  "kernel": str(kernel_copy),
                  "kernel_sha256": hashlib.sha256(kernel_copy.read_bytes()).hexdigest(),
                  "qemu_reboot_action":launch_reboot_action(args),
                  "gpu": os.environ.get("BROOK_GPU", "software"),
                  "render_node": os.environ.get("BROOK_RENDERNODE", "/dev/dri/renderD128"),
                  "cloned_disks": os.environ.get("BROOK_VM_CLONED_DISKS") == "1"}
    write_json(directory / "session.json", descriptor)
    print(f"Brook VM bridge session: {session}\nRegistry: {directory}", flush=True)

    def mirror():
        while True:
            chunk = child.stdout.read1(65536)
            if not chunk:
                break
            log.write(chunk)
            log.flush()
            try:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            except BrokenPipeError:
                pass

    thread = threading.Thread(target=mirror, daemon=True)
    thread.start()

    def expire_recordings():
        recording = directory / "recording.json"
        while child.poll() is None:
            time.sleep(.1)
            try:
                value = json.loads(recording.read_text())
                if time.monotonic() >= value["deadline"]:
                    with QMP(session) as q:
                        info = q.hmp("info capture")
                        if value["artifact"] in info:
                            q.hmp("stopcapture " + str(value["index"]))
                    recording.unlink(missing_ok=True)
            except (OSError, ValueError, RuntimeError, KeyError):
                pass
    threading.Thread(target=expire_recordings, daemon=True).start()

    def forward(signum, _):
        if child.poll() is None:
            child.send_signal(signum)
    signal.signal(signal.SIGTERM, forward)
    signal.signal(signal.SIGINT, forward)
    result = child.wait()
    thread.join(timeout=5)
    log.close()
    # A client may have changed the generation after an explicit guest reset.
    descriptor = json.loads((directory / "session.json").read_text())
    descriptor.update(state="stopped", stopped_at=time.time(), exit_code=result)
    write_json(directory / "session.json", descriptor)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--debug-port", type=int, required=True)
    parser.add_argument("--provenance", default="user")
    parser.add_argument("--scenario", default="existing-boot-script")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    cmd = options.command[1:] if options.command[:1] == ["--"] else options.command
    raise SystemExit(supervise(cmd, options.kernel, options.disk, options.debug_port,
                               options.provenance, options.scenario))

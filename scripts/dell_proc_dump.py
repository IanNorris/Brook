#!/usr/bin/env python3
"""Capture a live Brook process over an existing, boot-pinned debug session."""

import argparse
import json
import os
from pathlib import Path
import shlex
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/vm_bridge")]

from hardware import Hardware
from hardware_bulk_workflow import BulkRun

DEFAULT_KEY = Path("/data/Programming/Brook/shared/dell/hardware-update-key")


def capture(bridge, session, pid, output, key_file, port=1236, full=False):
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)
    artifact_id = uuid.uuid4().hex
    command = f"proc dump {pid} /data/.brook-transfer/{artifact_id}.bin"
    if full:
        command += " --full"
    with bridge.session_lock(session) as guest:
        result = bridge.guest(guest, command, timeout=300)
    if result.get("error") or not result.get("bytes") or not result.get("threads"):
        raise RuntimeError(f"{command} returned {result}")
    # Use a private transfer directory beside the output so the completed
    # artifact can be published without overwriting an existing local path.
    with tempfile.TemporaryDirectory(prefix=".brook-core-", dir=output.parent) as staging:
        receipt = BulkRun(bridge, Path(staging) / "transfer").execute(
            session, key_file, "getz", artifact_id=artifact_id, port=port
        )
        artifact = Path(receipt["artifact_path"])
        if artifact.stat().st_size != result["bytes"]:
            raise RuntimeError("Core size differs from the guest's completion reply")
        artifact.chmod(0o600)
        os.link(artifact, output)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session")
    parser.add_argument("pid", type=int)
    parser.add_argument("out", type=Path)
    parser.add_argument("--key-file", type=Path, default=DEFAULT_KEY)
    parser.add_argument("--registry", type=Path, help="Use this session registry")
    parser.add_argument("--bulk-port", type=int, default=1236)
    parser.add_argument("--full", action="store_true")
    parser.add_argument("--exe", type=Path, help="Matching host executable, if the guest path is unavailable")
    args = parser.parse_args()
    if not 1 <= args.pid <= 65535:
        parser.error("PID must be in 1..65535")
    try:
        result = capture(Hardware(args.registry), args.session, args.pid, args.out,
                         args.key_file, args.bulk_port, args.full)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(result))
    executable = args.exe or result.get("exe") or "<matching-executable>"
    print(shlex.join(["gdb", "-batch", "-iex", "set sysroot /",
                      "-ex", "thread apply all bt", str(executable), str(args.out.absolute())]))


if __name__ == "__main__":
    main()

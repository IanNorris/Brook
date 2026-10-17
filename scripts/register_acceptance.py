#!/usr/bin/env python3
"""Run the staged native register probe, retaining one resumable guest job.

This attaches to an existing experiment VM; it does not boot or stop that VM.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools/vm_bridge"))
import server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session")
    parser.add_argument("output", type=Path)
    parser.add_argument("--binary", default="/boot/REGCHK")
    parser.add_argument("--kernel", action="store_true", help="Also run root-only test-kernel probes")
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Keep runtime evidence under this repository's build directory")
    output.mkdir(parents=True, exist_ok=True)
    request_path = output / "request.json"
    identity = {"session": args.session, "binary": args.binary, "uid": 0 if args.kernel else 1000,
                "kernel": args.kernel}
    if request_path.exists():
        request = json.loads(request_path.read_text())
        if any(request.get(key, False if key == "kernel" else None) != value
               for key, value in identity.items()):
            parser.error("Output belongs to a different session/binary; use a fresh directory")
    else:
        request = {**identity, "request_id": uuid.uuid4().hex}
        with request_path.open("x") as stream:
            json.dump(request, stream, indent=2)
    # A timeout is not terminal. Rerunning with this output resumes the same ID.
    print("REGISTER_JOB", request["request_id"], flush=True)
    argv = [args.binary] + (["--kernel"] if args.kernel else [])
    result = server.guest_run(args.session, argv=argv, uid=identity["uid"],
                              request_id=request["request_id"], wait_s=15)
    deadline = time.monotonic() + 90
    terminal = {"completed", "failed", "rejected", "lost", "cancelled"}
    while True:
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if result["state"] in terminal:
            break
        if time.monotonic() >= deadline:
            raise TimeoutError("Job retained; rerun the same command to resume observation")
        result = server.job_status(args.session, request["request_id"], wait_s=10,
                                   output_limit=16384)
    text = (result.get("stdout") or {}).get("text", "")
    print(text, end="")
    error = (result.get("stderr") or {}).get("text", "")
    if error:
        print(error, file=sys.stderr, end="")
    exit_info = result.get("exit") or {}
    passed = (result["state"] == "completed" and exit_info.get("code") == 0
              and f"REGISTER_RESULT {11 if args.kernel else 10}/{11 if args.kernel else 10}" in text.splitlines())
    print("REGISTER_ACCEPTANCE", "PASS" if passed else "FAIL", result["state"])
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())

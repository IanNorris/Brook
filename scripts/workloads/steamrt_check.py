#!/usr/bin/env python3
"""Run the installed, unchanged SteamRT with bounded console evidence.

Run inside Brook as the desktop user. Full output stays in a unique guest
directory; the VM bridge receives paths, hashes and small tails only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def describe(path):
    size = path.stat().st_size
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
        stream.seek(max(0, size - 3072))
        tail = stream.read().decode(errors="replace")
    return dict(path=str(path), bytes=size, sha256=digest, tail=tail)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path, required=True)
    parser.add_argument("--no-copy-runtime", action="store_true",
                        help="Use pressure-vessel's supported diagnostic mode; default is unchanged")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("run as the desktop user")
    client = args.client.resolve(strict=True)
    runtime = client / "steamrt64/pv-runtime/steam-runtime-steamrt"
    platforms = list(runtime.glob("steamrt3c_platform_*/files"))
    if len(platforms) != 1:
        parser.error("expected exactly one installed SteamRT platform")
    platform = platforms[0]
    # /nix/profile is not one of the host-layout check's mounts. Use the real
    # installed coreutils directory, preserving argv[0] for its multicall true.
    corebin = Path("/nix/profile/bin/true").resolve(strict=True).parent
    true_program = corebin / "true"
    if not true_program.is_relative_to("/nix/store") or not true_program.exists():
        parser.error("installed coreutils true has no container-visible store path")
    env = dict(os.environ, HOME=str(client.parent / "user"),
               XDG_RUNTIME_DIR="/tmp", STEAM_RUNTIME_LOGGER="0",
               PRESSURE_VESSEL_VERBOSE="1",
               PATH=str(corebin) + ":" + os.environ.get("PATH", ""),
               LD_LIBRARY_PATH=":".join(map(str, [
                   client / "hostlibs", client / "steamrt64", client / "ubuntu12_64",
                   platform / "lib/x86_64-linux-gnu", platform / "usr/lib/x86_64-linux-gnu"])))
    for name in ["STEAM_RUNTIME", "STEAM_RUNTIME_STEAMRT", "PRESSURE_VESSEL_PREFIX", "LD_PRELOAD"]:
        env.pop(name, None)
    args.log_dir.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix="steamrt-", dir=args.log_dir))
    evidence.chmod(0o700)
    command = [str(runtime / "run"), "--batch"]
    if args.no_copy_runtime:
        command.append("--no-copy-runtime")
    command += ["--", "/bin/sh", "-c",
                "set -eu; printf '%s\\n' BROOK_STEAMRT_CHILD_STARTED; "
                "/usr/bin/getconf GNU_LIBC_VERSION; /bin/cat /etc/os-release"]
    request = dict(argv=command, uid=os.getuid(), evidence=str(evidence),
                   coreutils_true=str(true_program), no_copy_runtime=args.no_copy_runtime)
    (evidence / "request.json").write_text(json.dumps(request, indent=2) + "\n")
    print(json.dumps(request), flush=True)
    started = time.monotonic()
    # A unique directory makes append safe. Multiple forked writers must not
    # overwrite one another while Brook's shared file-offset work is pending.
    with (evidence / "stdout.log").open("ab") as stdout, (evidence / "stderr.log").open("ab") as stderr:
        result = subprocess.run(command, cwd=runtime, env=env, stdout=stdout, stderr=stderr)
    outcome = dict(exit=result.returncode, elapsed_s=time.monotonic() - started,
                   stdout=describe(evidence / "stdout.log"), stderr=describe(evidence / "stderr.log"))
    (evidence / "result.json").write_text(json.dumps(outcome, indent=2) + "\n")
    print(json.dumps(outcome), flush=True)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())

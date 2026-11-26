#!/usr/bin/env python3
"""Install only the network-independent Valve client seed into a Nix build output."""
import argparse
import hashlib
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap", type=Path, required=True)
    parser.add_argument("--patch-elf", required=True)
    parser.add_argument("--loader", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--plan", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.bootstrap.parent))
    from bootstrap import adapt_client, extract, has_interpreter

    plan = json.loads(args.plan.read_text())
    args.output.mkdir(parents=True, exist_ok=False)
    expanded = members = 0
    for package in plan["packages"]:
        archive = Path(package["store"])
        with archive.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if archive.stat().st_size != package["bytes"] or digest != package["sha256"]:
            raise ValueError("Valve archive size or SHA-256 mismatch: " + package["name"])
        expanded, members = extract(archive, args.output, expanded, members)

    client = args.output / "steamrt64/steam"
    with client.open("rb") as header:
        elf = header.read(20)
    if not has_interpreter(client) or elf[18:20] != b"\x3e\0":
        raise ValueError("The pinned archive does not contain an x86-64 Steam executable")
    runtime = args.output / "steamrt64/pv-runtime/steam-runtime-steamrt"
    provider_tools = runtime / "pressure-vessel/libexec/steam-runtime-tools-0"
    if (not (provider_tools / "srt-client-container").is_file() or
            not (provider_tools / "srt-bwrap").is_file() or
            not (runtime / "pressure-vessel/bin/steam-runtime-supervisor").is_file() or
            not (runtime / plan["runtime"] / "files").is_dir() or
            len(list(runtime.glob("steamrt3c_platform_*/files"))) != 1):
        raise ValueError("The pinned archive does not match the supported SteamRT3c layout")
    print(f"Steam {plan['version']}, {plan['runtime']}: "
          f"adapted {adapt_client(args.output, args.patch_elf, args.loader)} x86-64 interpreters")


if __name__ == "__main__":
    main()

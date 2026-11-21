#!/usr/bin/env python3
"""Compose a Brook app profile and build a fresh image for the selected target."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from image_profile import (
    ROOT, closure_receipt, data_size_mib, evaluate, nix_json, realize,
)
from kernel_capabilities import read_kernel_capabilities, require_kernel_capabilities


def csv_list(value):
    if value == "":
        return []
    items = value.split(",")
    if any(not item or item != item.strip() for item in items):
        raise argparse.ArgumentTypeError("Use a comma-separated list without empty entries")
    if len(set(items)) != len(items):
        raise argparse.ArgumentTypeError("List entries must be unique")
    return items


def size(value):
    if value == "auto":
        return None
    try:
        result = int(value)
        if result > 0:
            return result
    except ValueError:
        pass
    raise argparse.ArgumentTypeError("Use auto or a positive size in MiB")


def run(*args):
    subprocess.run(list(map(str, args)), cwd=ROOT, check=True)


def script(name, *args):
    run(sys.executable, ROOT / "scripts" / name, *args)


def release_build(build):
    if build is not None:
        return build.resolve(strict=True)
    run("bash", ROOT / "scripts/build.sh", "Release")
    return ROOT / "build/release"


def build_qemu(args):
    build = release_build(args.build)
    kernel = read_kernel_capabilities(build / "kernel/BROOK.elf")
    output = args.output.resolve()

    if args.apps is None and not args.packages:
        require_kernel_capabilities(kernel, {"chromium-sandbox": 1})
        if args.data_mib is not None:
            raise ValueError("Explicit sizing is available for a custom --apps selection")
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
        seed = output / "seed"
        script("create_experiment_disks.py", "--public", "--profile-expression",
               ROOT / "nix/release-packages.nix", "--output", seed)
        script("make_public_qemu.py", "--seed", seed, "--build", build,
               "--output", output / "image")
        return

    selection = args.selection
    require_kernel_capabilities(kernel, selection["manifest"]["requiredCapabilities"])
    profile = realize(args.config, selection)
    closure = closure_receipt(profile)
    capacity = data_size_mib(closure["nar_bytes"], args.data_mib, 512, 32768)
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    receipt = output / "profile-selection.json"
    receipt.write_text(json.dumps(selection, indent=2, sort_keys=True) + "\n")
    seed = output / "seed"
    script("prepare_hardware_store.py", "--profile", profile, "--profile-selection",
           receipt, "--direct-ahci", "--size-mib", capacity, "--output", seed)
    script("make_public_qemu.py", "--developer", "--seed", seed, "--build", build,
           "--output", output / "image")
    (output / "brook-image.json").write_text(json.dumps({
        "schema": "brook-image/1",
        "target": args.target,
        "profile": str(profile),
        "closure": closure,
        "data_mib": capacity,
        "kernel": kernel,
        "redistributable": selection["manifest"]["redistributable"],
        "image_manifest": "image/manifest.json",
    }, indent=2, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list-apps", action="store_true")
    parser.add_argument("--target", choices=("qemu",))
    parser.add_argument("--apps", type=csv_list, help="Comma-separated selection; empty selects the base")
    parser.add_argument("--packages", type=csv_list, default=[], help="Additional nixpkgs CLI packages")
    parser.add_argument("--data-mib", type=size, help="Store capacity, in MiB or auto (default)")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build", type=Path, help="Use this existing build instead of compiling Release")
    parser.add_argument("--allow-unfree", action="store_true", help="Allow approved unfree apps in a private image")
    parser.add_argument("--public-release", action="store_true", help="Require the fixed public QEMU profile")
    args = parser.parse_args()
    if args.list_apps:
        print(json.dumps(nix_json(ROOT / "nix/app-registry.nix", "-A", "metadata"),
                         indent=2, sort_keys=True))
        return
    if args.target is None or args.output is None:
        parser.error("--target and --output are required")
    if args.output.exists() or args.output.is_symlink():
        parser.error("--output must be a new directory")
    if args.public_release and (args.apps is not None or args.packages or args.allow_unfree):
        parser.error("The public release requires its fixed free-package profile")
    if args.allow_unfree and args.apps is None and not args.packages:
        parser.error("--allow-unfree requires a custom selection")
    if not os.environ.get("MUSL_CC"):
        parser.error("Run inside the pinned Nix shell")

    try:
        if args.apps is not None or args.packages:
            args.config = {
                "apps": args.apps or [],
                "extraPackageNames": args.packages,
                "target": args.target,
                "allowUnfree": args.allow_unfree,
                "publicRelease": args.public_release,
            }
            args.selection = evaluate(args.config)
        build_qemu(args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"brook_image: {error}\n")


if __name__ == "__main__":
    main()

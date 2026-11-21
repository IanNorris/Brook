#!/usr/bin/env python3
"""Compose a Brook app profile and build a fresh image for the selected target."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

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
    sizing = ["--auto-size"] if args.data_mib is None else []
    script("prepare_hardware_store.py", "--profile", profile, "--profile-selection",
           receipt, "--direct-ahci", "--size-mib", capacity, "--output", seed, *sizing)
    actual = json.loads((seed / "manifest.json").read_text())
    script("make_public_qemu.py", "--developer", "--seed", seed, "--build", build,
           "--output", output / "image")
    (output / "brook-image.json").write_text(json.dumps({
        "schema": "brook-image/1",
        "target": args.target,
        "profile": str(profile),
        "closure": closure,
        "data_mib": actual["bytes"] // (1024 * 1024),
        "kernel": kernel,
        "redistributable": selection["manifest"]["redistributable"],
        "image_manifest": "image/manifest.json",
    }, indent=2, sort_keys=True) + "\n")


def build_vmware(args):
    command = ["--output", args.output, "--operator-policy", args.operator_policy,
               "--recovery-checkers-from", args.recovery_checkers_from, "--network", args.network]
    if args.payload_id:
        command += ["--payload-id", args.payload_id]
    if args.data_mib is not None:
        command += ["--data-mib", args.data_mib]
    if args.autofit:
        command.append("--autofit")
    if args.apps is None and not args.packages:
        script("make_vmware_image.py", *command)
        return
    with TemporaryDirectory(prefix="brook-image-selection-") as temp:
        config = Path(temp) / "config.json"
        config.write_text(json.dumps(args.config))
        script("make_vmware_image.py", *command, "--selection-config", config)


def build_dell_pair(args):
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    pair = output / "pair"
    command = ["--output", pair, "--clearwater-shell",
               "--payload-id", args.payload_id or "brook-selected-dell"]
    if args.dell_shared is not None:
        command += ["--shared", args.dell_shared]
    if args.operator_policy is not None:
        command += ["--bridge-v3-operators", args.operator_policy]
    for option in args.kernel_option:
        command += ["--kernel-option", option]
    if args.apps is None and not args.packages:
        script("build_dell_image.py", *command)
        profile = Path(subprocess.check_output(
            ["nix-build", "--no-out-link", ROOT / "nix/hardware-network-packages.nix",
             "-A", "profile"], cwd=ROOT, text=True).strip())
        selection = None
    else:
        config_path = output / "selection-config.json"
        config_path.write_text(json.dumps(args.config, indent=2, sort_keys=True) + "\n")
        script("build_dell_image.py", *command, "--app-selection-config", config_path)
        selection = args.selection
        profile = realize(args.config, selection)
        (output / "profile-selection.json").write_text(
            json.dumps(selection, indent=2, sort_keys=True) + "\n")
    kernel = read_kernel_capabilities(pair / "kernel.elf")
    (output / "brook-image.json").write_text(json.dumps({
        "schema": "brook-image/1",
        "target": args.target,
        "profile": str(profile),
        "closure": closure_receipt(profile),
        "kernel": kernel,
        "redistributable": bool(selection and selection["manifest"]["redistributable"]),
        "image_manifest": "pair/build-manifest.json",
    }, indent=2, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list-apps", action="store_true")
    parser.add_argument("--target", choices=("qemu", "vmware", "dell-pair"))
    parser.add_argument("--apps", type=csv_list, help="Comma-separated selection; empty selects the base")
    parser.add_argument("--packages", type=csv_list, default=[], help="Additional nixpkgs CLI packages")
    parser.add_argument("--data-mib", type=size, help="Store capacity, in MiB or auto (default)")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build", type=Path, help="Use this existing build instead of compiling Release")
    parser.add_argument("--allow-unfree", action="store_true", help="Allow approved unfree apps in a private image")
    parser.add_argument("--public-release", action="store_true", help="Require the fixed public QEMU profile")
    parser.add_argument("--operator-policy", type=Path, help="VMware operator public keys")
    parser.add_argument("--recovery-checkers-from", type=Path, help="VMware pinned checker initrd")
    parser.add_argument("--network", choices=("hostonly", "nat"), default="hostonly")
    parser.add_argument("--autofit", action="store_true", help="Enable VMware host resize hints")
    parser.add_argument("--payload-id", help="Payload identifier for a guarded build")
    parser.add_argument("--dell-shared", type=Path, help="Private Dell pair source assets")
    parser.add_argument("--kernel-option", action="append", default=[],
                        help="Override a Dell pair kernel option (repeatable)")
    args = parser.parse_args()
    if args.list_apps:
        print(json.dumps(nix_json(ROOT / "nix/app-registry.nix", "-A", "metadata"),
                         indent=2, sort_keys=True))
        return
    if args.target is None or args.output is None:
        parser.error("--target and --output are required")
    if args.output.exists() or args.output.is_symlink():
        parser.error("--output must be a new directory")
    if args.target == "vmware" and (not args.operator_policy or not args.recovery_checkers_from):
        parser.error("VMware requires --operator-policy and --recovery-checkers-from")
    if args.target in ("vmware", "dell-pair") and args.build is not None:
        parser.error("The guarded target compiles its exact kernel")
    if args.target != "dell-pair" and (args.dell_shared or args.kernel_option):
        parser.error("Dell kernel settings require --target dell-pair")
    if args.target == "dell-pair" and args.data_mib is not None:
        parser.error("The Dell pair uses the existing persistent volume")
    if args.public_release and (args.target != "qemu" or args.apps is not None or args.packages or args.allow_unfree):
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
        {"qemu": build_qemu, "vmware": build_vmware, "dell-pair": build_dell_pair}[args.target](args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"brook_image: {error}\n")


if __name__ == "__main__":
    main()

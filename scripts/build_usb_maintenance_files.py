#!/usr/bin/env python3
"""Build the private Dell v3 USB maintenance FAT files and a host manifest.

Use the pinned dev shell. This writes only the requested output and build
directories. To migrate the stick, use migrate_usb_maintenance.py separately.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from bridge_keyless_image import require_keyless_disk, require_keyless_image
from bridge_operator_policy import validate_operator_policy
from build_dell_image import (DEFAULT_SHARED, ROOT, SOURCE_INITRD, SOURCE_INITRD_SHA,
                              build_provenance, embed_update_helpers, module_abi, run, sha)
from prepare_hardware_kernel_pair import prepare, regular

PATHS = ("KERNEL/BROOK.ELF", "INITRD.IMG", "EFI/BOOT/BOOTX64.EFI")
REMOVED = ("ahci-data-volume", "boot-app-image", "boot-app-snapshot",
           "writable-desktop", "usb-boot-poweroff")
INIT = (b"set wm\nset vfb none\nsched eevdf\nkconsole\n"
        b"run --console --uid 1000 /boot/HWSTATUS.ELF\n")
KERNEL_FLAGS = {
    "BROOK_BRIDGE_V3": "ON", "BROOK_QEMU_LEGACY_OUTBOUND_DEBUG": "OFF",
    "BROOK_SSD_GUARD_CHECK": "OFF", "BROOK_SSD_AUTO_RECOVERY": "OFF",
    "BROOK_SSD_GUARDED_DESKTOP": "OFF", "BROOK_SSD_INSTALLER": "OFF",
}


def fat_read(image, name):
    return subprocess.check_output(["mtype", "-i", image, "::/" + name])


def put(image, name, content):
    subprocess.run(["mcopy", "-o", "-i", image, "-", "::/" + name],
                   input=content, check=True)
    if fat_read(image, name) != content:
        raise ValueError(f"{name}: initrd readback mismatch")


def build(output, build_root, shared, operators, payload_id, epoch):
    if "HOST_CC" not in os.environ:
        raise ValueError("Run inside the pinned nix-shell")
    if os.path.lexists(output) or os.path.lexists(build_root):
        raise ValueError("Use new output and build directories")
    if not (ROOT / "src/third_party/fatfs/source/ff.c").is_file() or not (
            ROOT / "vendor/uefi-headers/Include/Uefi.h").is_file():
        raise ValueError("Initialize the pinned FAT and UEFI submodules first")
    policy = regular(operators, 1024)
    validate_operator_policy(policy)
    source = shared / SOURCE_INITRD
    if sha(source) != SOURCE_INITRD_SHA:
        raise ValueError("The source initrd does not match its pinned digest")
    commit = subprocess.check_output(["git", "-C", ROOT, "rev-parse", "HEAD"], text=True).strip()
    if subprocess.check_output(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"]):
        raise ValueError("Commit the tracked changes before building maintenance media")
    build_root.mkdir(mode=0o700, parents=True)
    operator_input = build_root / "operators.pub"
    operator_input.write_bytes(policy)
    provenance, assembly = build_provenance(commit, False, epoch)
    kernel_build = build_root / "kernel"
    loader_build = build_root / "loader"
    configure = [f"-D{name}:BOOL={value}" for name, value in KERNEL_FLAGS.items()]
    archive = shared / "uacpi-6.1.0.tar.gz"
    if archive.is_file():
        configure.append(f"-DBROOK_UACPI_ARCHIVE={archive}")
    run(["cmake", "-S", ROOT / "src/kernel", "-B", kernel_build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/kernel-x86_64-clang.cmake",
         f"-DBROOK_BRIDGE_V3_OPERATOR_KEYS_FILE:FILEPATH={operator_input}", *configure])
    run(["cmake", "--build", kernel_build])
    # Removable firmware boots this volume. SSD identity checks belong to the
    # maintenance kernel's AHCI mount, where the selected SATA disk is visible.
    run(["cmake", "-S", ROOT / "src/bootloader", "-B", loader_build, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/uefi-x86_64-clang.cmake"])
    run(["cmake", "--build", loader_build, "--target", "bootloader"])
    kernel = kernel_build / "BROOK.elf"
    with tempfile.TemporaryDirectory(dir=build_root) as scratch:
        pair = Path(scratch) / "pair"
        prepare(source, kernel_build, module_abi(), payload_id, pair, sha(kernel))
        image = pair / "INITRD.IMG"
        maintenance = {name: fat_read(image, "ETC/" + name) for name in
                       ("hardware-maintenance", "ahci-write-target", "ahci-boot-volume")}
        present = subprocess.check_output(["mdir", "-a", "-b", "-i", image, "::/ETC"], text=True).splitlines()
        for name in REMOVED:
            if "::/ETC/" + name in present:
                run(["mdel", "-i", image, "::/ETC/" + name])
        remaining = subprocess.check_output(["mdir", "-a", "-b", "-i", image, "::/ETC"], text=True).splitlines()
        if any("::/ETC/" + name in remaining for name in REMOVED):
            raise ValueError("Unexpected desktop or USB-reset selector in the initrd")
        put(image, "BOOT/INIT.RC", INIT)
        put(image, "ETC/build-provenance", provenance)
        helpers = embed_update_helpers(pair, kernel_build, operator_input)
        if any(fat_read(image, "ETC/" + name) != content for name, content in maintenance.items()):
            raise ValueError("The identity-bound maintenance settings changed")
        if fat_read(image, "ETC/bridge-v3-operators") != policy:
            raise ValueError("Bridge operator policy readback mismatch")
        require_keyless_image(image)
        sources = (pair / "kernel.elf", image, loader_build / "BOOTX64.efi")
        require_keyless_disk(sources[0])
        require_keyless_disk(sources[2])
        if sources[0].read_bytes()[:4] != b"\x7fELF" or sources[2].read_bytes()[:2] != b"MZ":
            raise ValueError("Unexpected ELF kernel or UEFI executable")
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output.parent, prefix=".usb-maintenance-") as staged:
            files = Path(staged) / "files"
            files.mkdir(mode=0o700)
            entries = {}
            for name, src in zip(PATHS, sources, strict=True):
                dst = files / name
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)
                entries[name] = {"bytes": dst.stat().st_size, "sha256": sha(dst)}
            manifest = {"format": "brook-usb-maintenance-v3", "commit": commit,
                        "payload_id": payload_id, "files": entries, "assembly": assembly,
                        "kernel_options": KERNEL_FLAGS, "source_initrd_sha256": SOURCE_INITRD_SHA,
                        "bridge_v3": {"version": 3, "policy_sha256": sha(operator_input),
                                      "helpers_sha256": helpers}}
            (files / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            # Destination must still be absent when the reviewed set is published.
            os.mkdir(output, mode=0o700)
            for item in files.iterdir():
                item.rename(output / item.name)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--shared", type=Path, default=DEFAULT_SHARED)
    parser.add_argument("--bridge-v3-operators", type=Path, required=True)
    parser.add_argument("--payload-id", required=True)
    parser.add_argument("--source-date-epoch", type=int)
    args = parser.parse_args()
    manifest = build(args.output.absolute(), args.build_root.absolute(), args.shared,
                     args.bridge_v3_operators, args.payload_id, args.source_date_epoch)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()

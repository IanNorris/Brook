"""Realize a selected Brook profile and describe its exact Nix closure."""

import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "brook-profile-selection/1"
MIB = 1024 * 1024
STORE_NAME = re.compile(r"[0-9a-df-np-sv-z]{32}-[A-Za-z0-9+._?=-]+\Z")


def store_path(value):
    path = Path(value)
    if path.parent != Path("/nix/store") or not STORE_NAME.fullmatch(path.name):
        raise ValueError(f"Invalid Nix store path: {value}")
    return path


def validate_selection(selection, profile=None):
    if selection.get("schema") != SCHEMA:
        raise ValueError("Unsupported Brook profile selection schema")
    selected = store_path(selection["profile"])
    if profile is not None and selected != store_path(profile):
        raise ValueError("Profile selection does not match the requested store root")
    if not isinstance(selection["packages"], list):
        raise ValueError("Profile selection requires a package list")
    for row in selection["packages"]:
        for key in ("name", "version", "store"):
            value = row[key]
            if not isinstance(value, str) or any(ord(c) < 32 for c in value):
                raise ValueError(f"Invalid profile package field: {key}")
        if not STORE_NAME.fullmatch(row["store"]):
            raise ValueError("Invalid profile package store name")
    if selection["intelVulkanDriver"] is not None:
        store_path(selection["intelVulkanDriver"])
    manifest = selection["manifest"]
    if not isinstance(manifest["redistributable"], bool):
        raise ValueError("Profile selection must declare its redistribution scope")
    launchers = manifest["launcherIds"]
    if not isinstance(launchers, list) or len(set(launchers)) != len(launchers):
        raise ValueError("Profile selection requires unique launcher IDs")
    if any(not re.fullmatch(r"[A-Za-z0-9_.-]+", item) for item in launchers):
        raise ValueError("Invalid profile launcher ID")
    return selection


def load_selection(path, profile=None):
    selection = json.loads(Path(path).read_text())
    return validate_selection(selection, profile)


def verify_realized_selection(selection):
    validate_selection(selection)
    profile = store_path(selection["profile"])
    if not profile.is_dir() or profile.resolve(strict=True) != profile:
        raise ValueError("Selected profile must be a realized Nix store directory")
    manifest = selection["manifest"]
    if manifest.get("preset") is not None:
        raise ValueError("Use the preset's original profile expression for compatibility seeds")
    try:
        embedded = json.loads((profile / "share/brook/profile.json").read_text())
    except (OSError, ValueError) as error:
        raise ValueError("Selected profile has no readable embedded manifest") from error
    if embedded != manifest:
        raise ValueError("Selected profile differs from its embedded manifest")
    if selection["packages"] != embedded.get("packages"):
        raise ValueError("Selected package metadata differs from the embedded manifest")
    return profile


def nix_json(*args):
    return json.loads(subprocess.check_output(
        ["nix-instantiate", "--eval", "--strict", "--json", *map(str, args)],
        cwd=ROOT, text=True,
    ))


def selection_args(config):
    return [ROOT / "nix/brook-image-selection.nix", "--argstr", "configJSON",
            json.dumps(config, sort_keys=True)]


def evaluate(config):
    selection = nix_json(*selection_args(config), "-A", "selection")
    return validate_selection(selection)


def realize(config, selection):
    paths = subprocess.check_output(
        ["nix-build", "--no-out-link", *map(str, selection_args(config)), "-A", "profile"],
        cwd=ROOT, text=True,
    ).splitlines()
    if len(paths) != 1 or store_path(paths[0]) != store_path(selection["profile"]):
        raise ValueError("Realized Nix profile differs from the evaluated selection")
    return Path(paths[0])


def closure_receipt(profile):
    entries = json.loads(subprocess.check_output(
        ["nix", "--extra-experimental-features", "nix-command", "path-info",
         "--json", "--recursive", str(store_path(profile))], cwd=ROOT, text=True,
    ))
    if isinstance(entries, list):
        entries = {entry["path"]: entry for entry in entries}
    closure = {str(store_path(path)): {"narHash": entry["narHash"], "narSize": entry["narSize"]}
               for path, entry in sorted(entries.items())}
    if str(profile) not in closure or any(
        type(row["narSize"]) is not int or row["narSize"] <= 0 for row in closure.values()
    ):
        raise ValueError("Incomplete or invalid realized Nix closure")
    canonical = json.dumps(closure, sort_keys=True, separators=(",", ":")).encode()
    return {
        "paths": len(closure),
        "nar_bytes": sum(row["narSize"] for row in closure.values()),
        "fingerprint_sha256": hashlib.sha256(canonical).hexdigest(),
    }


def data_size_mib(nar_bytes, requested, minimum, maximum, free_mib=0):
    # Provision room for ext2 metadata, materialized profile links and user data.
    # free_mib adds explicit headroom for installs; unused ext2 blocks stay
    # zero, so sparse disk formats (e.g. monolithicSparse VMDK) don't grow.
    if requested is not None:
        if not minimum <= requested <= maximum:
            raise ValueError(f"Data size must be between {minimum} and {maximum} MiB")
        return requested
    if free_mib < 0:
        raise ValueError("Free space must not be negative")
    required = max(minimum, (5 * nar_bytes + 4 * MIB - 1) // (4 * MIB) + 512 + free_mib)
    if required > maximum:
        raise ValueError(f"Profile needs at least {required} MiB; target limit is {maximum} MiB")
    return required

#!/usr/bin/env python3
"""Publish a Brook profile closure and a detached signature for its stage plan."""

import argparse
import concurrent.futures
import json
import subprocess
import sys
import urllib.error
import urllib.request
from urllib.parse import quote
from pathlib import Path

from image_profile import load_selection, verify_realized_selection

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/nix-install"))
from stage_nar import STORE, UPSTREAM, layout_digest, nar_hash, profile_layout, store_name
from stage_selected_launchers import validate_launcher_ids

EXPRESSION = ROOT / "nix/hardware-network-packages.nix"
NIX = ["nix", "--extra-experimental-features", "nix-command"]


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def closure_info(roots):
    result = run([*NIX, "path-info", "--json", "--recursive", *roots],
                 stdout=subprocess.PIPE, text=True)
    info = json.loads(result.stdout)
    if isinstance(info, list):
        info = {entry["path"]: entry for entry in info}
    return {
        store_name(path): {
            "narHash": nar_hash(entry["narHash"]),
            "narSize": entry["narSize"],
            "references": sorted(store_name(ref) for ref in entry["references"]),
        }
        for path, entry in info.items()
    }


def upstream_has(name, entry):
    url = f"{UPSTREAM}/{name[:32]}.narinfo"
    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            fields = dict(
                line.split(": ", 1)
                for line in response.read(65536).decode("utf-8").splitlines()
                if ": " in line
            )
    except urllib.error.HTTPError as error:
        if error.code not in (403, 404):
            raise
        return False
    return (
        fields.get("StorePath") == str(STORE / name)
        and nar_hash(fields["NarHash"]) == entry["narHash"]
        and int(fields["NarSize"]) == entry["narSize"]
        and sorted(fields.get("References", "").split()) == entry["references"]
    )


def generate_plan(profile, extras, cache_url, mirror_upstream, packages=(), *, launcher_ids=None):
    if launcher_ids is not None:
        validate_launcher_ids(launcher_ids)
    roots = list(dict.fromkeys(
        [str(profile), *map(str, extras), *(str(STORE / row["store"]) for row in packages)]
    ))
    closure = closure_info(roots)
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        checks = pool.map(lambda name: upstream_has(name, closure[name]), sorted(closure))
        for name, upstream in zip(sorted(closure), checks):
            closure[name]["source"] = "upstream" if upstream else "brook"
    plan = {
        "format": 2,
        "roots": [store_name(path) for path in roots],
        "closure": closure,
        "profile": {
            "root": store_name(str(profile)),
            "layoutSha256": layout_digest(profile_layout(profile, closure)),
        },
        "packages": list(packages),
        "cache": {"brook": cache_url, "mirrorUpstream": mirror_upstream},
    }
    if launcher_ids is not None:
        plan["format"] = 3
        plan["profile"]["launcherIds"] = launcher_ids
    return plan


def publish_cache(info, directory, key, mirror_upstream):
    names = [name for name, entry in sorted(info.items())
             if mirror_upstream or entry["source"] == "brook"]
    directory.mkdir()
    if names:
        url = directory.as_uri() + "?compression=zstd&secret-key=" + quote(str(key))
        # Standard Nix caches admit a path only after its references. Populate
        # those dependencies first, then retain the signed archives selected
        # by the plan. A cache served to a host-only target keeps its mirror.
        for start in range(0, len(names), 128):
            batch = [str(STORE / name) for name in names[start:start + 128]]
            run([*NIX, "copy", "--no-check-sigs", "--to", url, *batch])
    else:
        (directory / "nix-cache-info").write_text("StoreDir: /nix/store\nWantMassQuery: 0\n")
    retained = set()
    archives = set()
    for name in names:
        narinfo = directory / f"{name[:32]}.narinfo"
        fields = narinfo.read_text().splitlines()
        if not any(line.startswith("Sig: ") for line in fields):
            raise ValueError(f"Cache path was not signed: {name}")
        retained.add(narinfo)
        archives.add(directory / next(line.removeprefix("URL: ") for line in fields
                                      if line.startswith("URL: ")))
    for narinfo in directory.glob("*.narinfo"):
        if narinfo not in retained:
            narinfo.unlink()
    for archive in (directory / "nar").glob("*"):
        if archive.is_file() and archive not in archives:
            archive.unlink()


def verify_private_key(path):
    if not path.is_file() or path.stat().st_mode & 0o077:
        raise ValueError(f"Expected a pre-provisioned private key with mode 0600: {path}")


def profile_metadata(expression):
    query = (
        "let e = import (builtins.toPath " + json.dumps(str(expression))
        + ") {}; in { profile = toString e.profile; packages = map (p: "
        '{name = p.pname or p.name; version = p.version or ""; '
        "store = builtins.baseNameOf (toString p);}) e.packages; }"
    )
    metadata = run(["nix-instantiate", "--eval", "--strict", "--json", "-E", query],
                   stdout=subprocess.PIPE, text=True)
    return json.loads(metadata.stdout)


def selected_profile(receipt, requested=None):
    selection = load_selection(receipt, requested)
    profile = verify_realized_selection(selection)
    exported = {entry.stem for entry in (profile / "share/applications").glob("*.desktop")
                if entry.is_file()}
    launcher_ids = selection["manifest"]["launcherIds"]
    validate_launcher_ids(launcher_ids)
    if exported != set(launcher_ids):
        raise ValueError("Selected profile's desktop files differ from its launcher manifest")
    return profile, selection["packages"], launcher_ids


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, help="defaults to the network hardware profile")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--expression", type=Path,
                        help="profile expression providing package names and versions")
    source.add_argument("--profile-selection", type=Path,
                        help="realized profile-selection.json from brook_image.py "
                             "(requires a guest helper installed with --selected-profiles)")
    parser.add_argument("extras", nargs="*", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    delivery = parser.add_mutually_exclusive_group(required=True)
    delivery.add_argument("--cache-url", help="URL at which the cache/ output will be served")
    delivery.add_argument("--bridge-cache", action="store_true",
                          help="push the generated cache with dell_deploy.py, without a host HTTP server")
    parser.add_argument("--cache-key", required=True, type=Path, help="Nix binary-cache signing key")
    parser.add_argument("--plan-key", required=True, type=Path, help="Ed25519 private key in PEM format")
    parser.add_argument("--mirror-upstream", action="store_true",
                        help="publish upstream paths too for a device that can reach only the build host")
    args = parser.parse_args()

    for key in (args.cache_key, args.plan_key):
        verify_private_key(key)
    launcher_ids = None
    if args.profile_selection is not None:
        try:
            profile, packages, launcher_ids = selected_profile(args.profile_selection, args.profile)
        except (KeyError, OSError, TypeError, ValueError) as error:
            parser.error(f"Invalid profile selection: {error}")
    else:
        expression = (args.expression or EXPRESSION).resolve(strict=True)
        if args.profile is None:
            built = run(["nix-build", str(expression), "-A", "profile", "--no-out-link"],
                        stdout=subprocess.PIPE, text=True)
            profile = Path(built.stdout.strip())
        else:
            profile = args.profile
        profile = profile.resolve(strict=True)
        packages = []
        if args.profile is None or args.expression is not None:
            metadata = profile_metadata(expression)
            if str(profile) != metadata["profile"]:
                parser.error("The profile does not match the expression")
            packages = metadata["packages"]
    extras = [path.resolve(strict=True) for path in args.extras]
    output = args.output.absolute()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    cache_url = args.cache_url.rstrip("/") if args.cache_url else None
    plan = generate_plan(profile, extras, cache_url, args.mirror_upstream, packages,
                         launcher_ids=launcher_ids)
    publish_cache(plan["closure"], output / "cache", args.cache_key.resolve(), args.mirror_upstream)
    document = output / "stage-plan.json"
    document.write_text(json.dumps(plan, sort_keys=True, separators=(",", ":")) + "\n")
    run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(args.plan_key),
         "-in", str(document), "-out", str(output / "stage-plan.sig")])
    print(document)


if __name__ == "__main__":
    main()

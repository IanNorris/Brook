"""Apply verified Nix closures as Brook desktop profile generations."""

import argparse
import contextlib
import ctypes
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlsplit

from stage_nar import STORE, STORE_NAME, ensure_path, nar_hash, rename_no_replace
from collect_desktop_entries import collect

MAX_PLAN = 16 * 1024 * 1024
DESKTOP_UID = 1000
GENERATION = re.compile(r"[.]brook-profile-(?:legacy|[0-9a-f]{32})")
TX = re.compile(r"[0-9a-f]{32}")
LAUNCHER = ".brook-launcher"
APP_ALIAS = "../profile/" + LAUNCHER


def safe_name(value):
    if type(value) is not str or not STORE_NAME.fullmatch(value):
        raise ValueError(f"Invalid store name {value!r}")
    return value


def relative_path(value):
    pieces = value.split("/")
    if any(not part or part in (".", "..") or any(ord(c) < 32 for c in part) for part in pieces):
        raise ValueError(f"Invalid profile path {value!r}")
    if pieces[0] in (LAUNCHER, "manifest.tsv", ".mutation-lock"):
        raise ValueError(f"Reserved profile path {value!r}")
    return Path(value)


def validate_plan(plan):
    if plan["format"] != 1:
        raise ValueError("Unknown closure plan version")
    cache = urlsplit(plan["cache"]["brook"])
    if (cache.scheme not in ("http", "https") or not cache.hostname
            or cache.username is not None or cache.query or cache.fragment):
        raise ValueError("Invalid Brook cache address")
    if type(plan["cache"]["mirrorUpstream"]) is not bool:
        raise ValueError("mirrorUpstream must be a boolean")
    closure = plan["closure"]
    for name, entry in closure.items():
        safe_name(name)
        if entry["narHash"] != nar_hash(entry["narHash"]) or entry["source"] not in ("brook", "upstream"):
            raise ValueError(f"Invalid archive metadata for {name}")
        if type(entry["narSize"]) is not int or not 0 < entry["narSize"] <= (64 << 30):
            raise ValueError(f"Invalid NAR size for {name}")
        if entry["references"] != sorted(set(entry["references"])) or not set(entry["references"]) <= closure.keys():
            raise ValueError(f"Missing or ambiguous references for {name}")
    pending = list(plan["roots"])
    reachable = set()
    while pending:
        name = pending.pop()
        if name not in reachable:
            pending.extend(closure[name]["references"])
            reachable.add(name)
    if not reachable or reachable != closure.keys():
        raise ValueError("Plan contains paths outside the requested closure")
    layout = plan["profile"]
    directories = {relative_path(item) for item in layout["directories"]}
    links = {relative_path(item) for item in layout["links"]}
    if links & directories or any(p.parent != Path() and p.parent not in directories
                                 for p in directories | links):
        raise ValueError("Invalid profile tree")
    for target in layout["links"].values():
        suffix = target.removeprefix("/nix/store/")
        name, separator, remainder = suffix.partition("/")
        if not target.startswith("/nix/store/") or name not in closure:
            raise ValueError(f"Profile target outside the closure: {target}")
        if separator:
            relative_path(remainder)
    for package in plan["packages"]:
        if package["store"] not in closure:
            raise ValueError("Package row outside the closure")
        checked_row(package["name"], package["version"], package["store"])


def checked_row(name, version, store):
    if not name or any(c in name + version for c in "\0\t\r\n"):
        raise ValueError("Invalid profile package row")
    return name, version, safe_name(store)


def verified_plan(document, signature, key, openssl):
    # Parse the same bytes that are verified, even if the transfer directory
    # belongs to the desktop user.
    with document.open("rb") as stream:
        data = stream.read(MAX_PLAN + 1)
    with signature.open("rb") as stream:
        sig = stream.read(65)
    if len(data) > MAX_PLAN or len(sig) != 64:
        raise ValueError("Invalid closure plan or Ed25519 signature length")
    with tempfile.TemporaryDirectory(prefix="brook-stage-verification-") as scratch:
        saved = Path(scratch)
        (saved / "plan").write_bytes(data)
        (saved / "sig").write_bytes(sig)
        subprocess.run([openssl, "pkeyutl", "-verify", "-pubin", "-rawin", "-inkey", key,
                        "-in", saved / "plan", "-sigfile", saved / "sig"],
                       check=True, stdout=subprocess.DEVNULL)
    plan = json.loads(data)
    validate_plan(plan)
    return plan, hashlib.sha256(data).hexdigest()


def flush_directory(path):
    with contextlib.ExitStack() as stack:
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        stack.callback(os.close, fd)
        os.fsync(fd)


def durable_text(path, text):
    with path.open("x") as stream:
        stream.write(text)
        stream.flush()
        os.fsync(stream.fileno())
    flush_directory(path.parent)


def active_generation(nix):
    current = os.readlink(nix / "profile")
    if not GENERATION.fullmatch(current) or not (nix / current).is_dir():
        raise ValueError("Invalid /nix/profile generation")
    apps = nix / "share/applications"
    if os.readlink(apps) != APP_ALIAS or not apps.is_dir():
        raise ValueError("Launcher index is not attached to the profile generation")
    return current


def switch(nix, generation):
    if not GENERATION.fullmatch(generation) or not (nix / generation).is_dir():
        raise ValueError("Invalid profile generation name")
    temp = nix / ".brook-profile-next"
    temp.unlink(missing_ok=True)
    temp.symlink_to(generation)
    os.replace(temp, nix / "profile")
    flush_directory(nix)


def manifest_rows(profile):
    manifest = profile / "manifest.tsv"
    if manifest.is_file():
        for line in manifest.read_text().splitlines():
            if line:
                yield checked_row(*line.split("\t"))


def collect_user_overlay(nix, old):
    """Keep exports made by nix-install, whose store symlinks are relative."""
    store = nix / "store"
    direct = set()
    links = {}
    for prefix in ("bin", "lib", "share/applications"):
        directory = old / prefix
        if not directory.is_dir() or directory.is_symlink():
            continue
        root = "../" * (prefix.count("/") + 2) + "store/"
        for export in directory.iterdir():
            if not export.is_symlink():
                continue
            value = os.readlink(export)
            if not value.startswith(root):
                continue
            name, separator, suffix = value[len(root):].partition("/")
            if not separator or not STORE_NAME.fullmatch(name) or suffix != prefix + "/" + export.name:
                continue
            if not (store / name / suffix).is_file():
                continue
            links[export.relative_to(old)] = value
            direct.add(name)
    rows = [row for row in manifest_rows(old) if row[2] in direct]
    return rows, links


def prepare(nix, plan, previous, destination):
    old = nix / previous
    rows, overlay = collect_user_overlay(nix, old)
    manifest = {p["name"].casefold(): (p["name"], p["version"], p["store"])
                for p in plan["packages"]}
    manifest.update({row[0].casefold(): row for row in rows})
    for path in sorted(plan["profile"]["directories"], key=lambda item: (item.count("/"), item)):
        (destination / path).mkdir()
    for name, target in plan["profile"]["links"].items():
        store_path = nix / target.removeprefix("/nix/")
        if not store_path.is_file():
            raise ValueError(f"Profile target is missing: {target}")
        link = destination / name
        # Use relative store links in fake roots too, without confusing them
        # with the Gallery-installed relative links tracked above.
        link.symlink_to(target if nix == Path("/nix") else store_path)
    for name, target in overlay.items():
        export = destination / name
        export.parent.mkdir(parents=True, exist_ok=True)
        export.unlink(missing_ok=True)
        export.symlink_to(target)
    durable_text(destination / "manifest.tsv", "".join(
        "\t".join(row) + "\n" for row in sorted(manifest.values())))
    launcher = destination / LAUNCHER
    (destination / "share/applications").mkdir(parents=True, exist_ok=True)
    collect(nix / "store", launcher, profile=destination, hardware=True)
    if nix == Path("/nix"):
        for directory, _, files in os.walk(destination, followlinks=False):
            os.chown(directory, DESKTOP_UID, DESKTOP_UID)
            for file in files:
                os.chown(Path(directory) / file, DESKTOP_UID, DESKTOP_UID, follow_symlinks=False)
    os.sync()


@contextlib.contextmanager
def mutation_lock(nix):
    lock = nix / "var/.brook-profile-lock"
    lock.mkdir(mode=0o700)
    try:
        (lock / "owner").write_text(f"pid={os.getpid()} uid={os.getuid()}\n")
        yield
    finally:
        (lock / "owner").unlink(missing_ok=True)
        lock.rmdir()


def notify(nix):
    if nix == Path("/nix"):
        ctypes.CDLL(None).syscall(0xB03)


def apply(nix, plan, digest, transaction, tools):
    state = nix / ".brook-stage"
    state.mkdir(mode=0o700, exist_ok=True)
    target = ".brook-profile-" + transaction
    record = state / (transaction + ".json")
    with mutation_lock(nix):
        previous = active_generation(nix)
        if record.is_file():
            existing = json.loads(record.read_text())
            if existing["digest"] != digest:
                raise ValueError("Transaction ID belongs to a different closure plan")
            if previous == target:
                return
            if previous != existing["previous"]:
                raise ValueError("Another profile is active; refusing to overwrite it")
            # A prepared generation and journal are durable before publication.
        counts = {"present": 0, "upstream": 0, "brook": 0}
        for name, entry in sorted(plan["closure"].items()):
            result = ensure_path(nix / "store", name, entry, plan["cache"], tools)
            counts[result] += 1
        print(json.dumps({"closure": counts}), flush=True)
        if not record.is_file():
            if (nix / target).exists():
                raise ValueError("Unjournaled generation requires inspection")
            # Both staging and live generation must be at the same depth.
            temp = Path(tempfile.mkdtemp(prefix=".brook-profile-building-", dir=nix))
            try:
                prepare(nix, plan, previous, temp)
                temp.chmod(0o755)
                rename_no_replace(temp, nix / target)
                flush_directory(nix)
            finally:
                if temp.exists():
                    shutil.rmtree(temp)
            durable_text(record, json.dumps({"previous": previous, "digest": digest}) + "\n")
        switch(nix, target)
    notify(nix)


def rollback(nix, transaction):
    with mutation_lock(nix):
        path = nix / ".brook-stage" / (transaction + ".json")
        previous = json.loads(path.read_text())["previous"]
        current = active_generation(nix)
        if current == previous:
            return
        if current != ".brook-profile-" + transaction:
            raise ValueError("A newer profile is active; refusing to roll it back")
        switch(nix, previous)
    notify(nix)


def initialize(nix):
    """Migrate a quiescent legacy image. Re-entry resumes an interrupted migration."""
    with mutation_lock(nix):
        profile = nix / "profile"
        previous = nix / ".brook-profile-legacy"
        apps = nix / "share/applications"
        apps_previous = apps.with_name(".brook-applications-legacy")
        if profile.is_symlink() and apps.is_symlink():
            active_generation(nix)
            return
        if not previous.exists():
            if profile.is_symlink() or not profile.is_dir() or apps.is_symlink() or not apps.is_dir():
                raise ValueError("Legacy profile or launcher directory missing")
            launcher = profile / LAUNCHER
            if not launcher.exists():
                with tempfile.TemporaryDirectory(prefix=".brook-app-copy-", dir=profile) as scratch:
                    copy = Path(scratch) / "launcher"
                    shutil.copytree(apps, copy, symlinks=True)
                    if nix == Path("/nix"):
                        for directory, _, files in os.walk(copy, followlinks=False):
                            os.chown(directory, DESKTOP_UID, DESKTOP_UID)
                            for file in files:
                                os.chown(Path(directory) / file, DESKTOP_UID, DESKTOP_UID,
                                         follow_symlinks=False)
                    os.sync()
                    rename_no_replace(copy, launcher)
                    flush_directory(profile)
            rename_no_replace(profile, previous)
            flush_directory(nix)
        if not profile.is_symlink():
            switch(nix, previous.name)
        if not apps.is_symlink():
            if apps.exists():
                rename_no_replace(apps, apps_previous)
            apps.symlink_to(APP_ALIAS)
            flush_directory(apps.parent)
        active_generation(nix)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("apply", "rollback", "initialize"))
    parser.add_argument("transaction", nargs="?")
    parser.add_argument("plan", nargs="?", type=Path)
    parser.add_argument("signature", nargs="?", type=Path)
    args = parser.parse_args()
    if args.action != "initialize" and (not args.transaction or not TX.fullmatch(args.transaction)):
        parser.error("Expected a 32-digit hexadecimal transaction ID")
    fake = os.environ.get("BROOK_STAGE_ROOT")
    nix = Path(fake).resolve() / "nix" if fake else Path("/nix")
    if not fake and os.geteuid() != 0:
        parser.error("Closure staging requires the maintenance operator")
    if args.action == "initialize":
        initialize(nix)
    elif args.action == "rollback":
        rollback(nix, args.transaction)
    else:
        if not args.plan or not args.signature:
            parser.error("Apply requires the plan and its signature")
        key = nix / "etc/brook/stage-plan.pub"
        if fake:
            tools = {name: shutil.which(name) or str(nix / "bin" / name)
                     for name in ("openssl", "curl", "xz", "zstd", "nar-unpack")}
        else:
            tools = {name: str(nix / "bin" / name) for name in ("curl", "xz", "zstd", "nar-unpack")}
            tools["openssl"] = os.environ["BROOK_STAGE_OPENSSL"]
        plan, digest = verified_plan(args.plan, args.signature, key, tools["openssl"])
        apply(nix, plan, digest, args.transaction, tools)
    print("PROFILE_" + args.action.upper() + "_OK " + (args.transaction or ""))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"nix stage-profile: {error}", file=sys.stderr)
        sys.exit(1)

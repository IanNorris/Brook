"""One-time maintenance migration. Runs under the existing minimal Python."""

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

from stage_nar import STORE_NAME, verify_tree

PROFILE_ALIAS = "var/.brook-profiles/current"
APP_ALIAS = "../profile/.brook-launcher"
LEGACY = ".brook-profile-legacy"
BOOTSTRAP = "etc/brook/.profile-stage-bootstrap.json"
REQUIRED = ("brook-run", "brook-chromium", "brook-firefox", "brook-webapp", "brook-apps-index")
CHUNK = 1024 * 1024


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_file(path, read, mode):
    fd, name = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            while block := read(CHUNK):
                stream.write(block)
            os.fchmod(stream.fileno(), mode)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
        fsync_dir(path.parent)
    finally:
        Path(name).unlink(missing_ok=True)


def save(path, data):
    write_file(path, io.BytesIO((json.dumps(data, sort_keys=True) + "\n").encode()).read, 0o600)


def link_equals(path, value):
    return path.is_symlink() and os.readlink(path) == value


def aliases(nix):
    return (link_equals(nix / "profile", PROFILE_ALIAS)
            and link_equals(nix / "share/applications", APP_ALIAS))


def read_record(nix):
    path = nix / BOOTSTRAP
    if path.is_symlink():
        raise ValueError("Unexpected bootstrap record type")
    return json.loads(path.read_text()) if path.is_file() else None


def manifest_data(path):
    if path.stat().st_size > 5 * 1024 * 1024:
        raise ValueError("Bootstrap manifest exceeds 5 MiB")
    doc = json.loads(path.read_text())
    if doc["format"] != 1 or doc["helper"] not in doc["closure"]:
        raise ValueError("Unsupported bootstrap manifest")
    for name in doc["closure"]:
        if not STORE_NAME.fullmatch(name):
            raise ValueError(f"Invalid bootstrap store entry {name!r}")
    return doc


def trusted_file(path, *, executable=False):
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o022
            or (executable and not info.st_mode & 0o100)):
        raise ValueError(f"Unexpected maintenance file {path}")


def probe(nix, manifest, identity):
    old = read_record(nix)
    if old and old["identity"] != identity:
        raise ValueError("A different bootstrap transaction already exists")
    if old and old["phase"] == "rolled_back":
        raise ValueError("Bootstrap was rolled back. Inspect the previous receipt before retrying")
    if not all((nix / name).is_dir() and not (nix / name).is_symlink()
               for name in ("bin", "store", "share", "var", "etc")):
        raise ValueError("Unexpected Nix system directories")
    nix_install = nix / "bin/nix-install"
    trusted_file(nix_install, executable=True)
    trusted_file(nix / "bin/nar-unpack", executable=True)
    if not link_equals(nix / "bin/nix", "nix-install"):
        raise ValueError("Unexpected /nix/bin/nix launcher")
    profile = nix / "profile"
    apps = nix / "share/applications"
    if not old:
        if any(not path.is_dir() or path.is_symlink() for path in (profile, apps)):
            raise ValueError("Expected legacy materialised profile and launcher paths")
        if (profile / ".mutation-lock").exists() or (nix / "var/.brook-profile-lock").exists():
            raise ValueError("An installer currently owns the profile")
        for binary in REQUIRED:
            if not (profile / "bin" / binary).is_file():
                raise ValueError(f"Network profile signal missing: {binary}")
        if not (apps / "applications.idx").is_file():
            raise ValueError("Current launcher index is missing")
    elif old["phase"] == "complete" and not aliases(nix):
        raise ValueError("Completed bootstrap aliases changed")
    helper = nix / "bin/brook-stage-profile"
    expected_helper = f"/nix/store/{manifest['helper']}/bin/brook-stage-profile"
    if os.path.lexists(helper) and not (old and link_equals(helper, expected_helper)):
        raise ValueError("Unexpected existing stage helper")
    key = nix / "etc/brook/stage-plan.pub"
    preexisting = os.path.lexists(key)
    if preexisting:
        trusted_file(key)
        if digest(key) != manifest["keySha256"]:
            raise ValueError("An unrelated stage signing key is installed")
    previous = digest(nix_install)
    if old and previous not in (old["oldInstaller"], manifest["installerSha256"]):
        raise ValueError("Installed nix changed after the bootstrap began")
    missing = []
    for index, (name, entry) in enumerate(manifest["closure"].items()):
        path = nix / "store" / name
        if os.path.lexists(path):
            verify_tree(path, entry)
        else:
            missing.append(index)
    managed = aliases(nix)
    if old and old["phase"] == "complete" and (
            missing or not preexisting or not helper.is_symlink()
            or previous != manifest["installerSha256"]):
        raise ValueError("The completed bootstrap runtime or key has changed")
    return {"kind": "managed" if managed else "legacy", "missing": missing,
            "generation": os.readlink(nix / PROFILE_ALIAS) if managed else None,
            "installedNix": previous,
            "keyPresent": preexisting,
            "phase": old["phase"] if old else "new"}


def checked_member(tar, name, expected_hash, size_limit, target, mode):
    info = tar.getmember(name)
    if not info.isfile() or info.size > size_limit:
        raise ValueError(f"Unexpected bootstrap archive member {name}")
    with tar.extractfile(info) as source:
        write_file(target, source.read, mode)
    if digest(target) != expected_hash:
        raise ValueError(f"Wrong hash for {name}")


def import_runtime(nix, manifest, archive, missing):
    with tarfile.open(archive, "r:") as tar, tempfile.TemporaryDirectory(
            dir=nix / "store", prefix=".brook-stage-bootstrap-") as scratch:
        work = Path(scratch)
        installer, key = work / "nix-install", work / "stage-plan.pub"
        checked_member(tar, "nix-install", manifest["installerSha256"], 16 << 20, installer, 0o755)
        checked_member(tar, "stage-plan.pub", manifest["keySha256"], 4096, key, 0o644)
        ordered = list(manifest["closure"].items())
        for index in missing:
            name, entry = ordered[index]
            item = tar.getmember(name + ".nar")
            if not item.isfile() or item.size != entry["narSize"]:
                raise ValueError(f"Unexpected NAR length for {name}")
            with tar.extractfile(item) as source:
                raw = work / "nar"
                with raw.open("xb") as output:
                    shutil.copyfileobj(source, output, CHUNK)
            if digest(raw) != entry["hexNarHash"]:
                raise ValueError(f"Wrong archive hash for {name}")
            unpacked = work / "entry"
            with raw.open("rb") as source:
                subprocess.run([nix / "bin/nar-unpack", unpacked], check=True, stdin=source)
            verify_tree(unpacked, entry)
            destination = nix / "store" / name
            if os.path.lexists(destination):
                verify_tree(destination, entry)
                if unpacked.is_dir() and not unpacked.is_symlink():
                    shutil.rmtree(unpacked)
                else:
                    unpacked.unlink()
            else:
                os.rename(unpacked, destination)
                fsync_dir(destination.parent)
            raw.unlink()
        return installer.read_bytes(), key.read_bytes()


def remove_own_lock(nix, identity):
    for lock in (nix / "profile/.mutation-lock",
                 nix / PROFILE_ALIAS.rsplit("/", 1)[0] / LEGACY / ".mutation-lock"):
        owner = lock / "owner"
        if owner.is_file() and owner.read_text() == identity + "\n":
            owner.unlink()
            lock.rmdir()
            fsync_dir(lock.parent)


def apply(nix, manifest, identity, archive, check, fake):
    current = probe(nix, manifest, identity)
    record = read_record(nix)
    if record and record["phase"] == "complete":
        if current["missing"] or current["installedNix"] != manifest["installerSha256"]:
            raise ValueError("The completed bootstrap runtime has changed")
        return current
    if current != check:
        raise ValueError("The current device state differs from the bootstrap preflight")
    house = nix / "etc/brook"
    house.mkdir(mode=0o755, exist_ok=True)
    if house.is_symlink():
        raise ValueError("Unexpected trusted key directory")
    mutex = house / ".profile-stage-bootstrap-lock"
    mutex.mkdir(mode=0o700)
    try:
        legacy = nix / "profile/.mutation-lock"
        if not record:
            legacy.mkdir(mode=0o700)
            (legacy / "owner").write_text(identity + "\n")
            try:
                old = nix / "bin/nix-install"
                backup = house / ".nix-install-before-stage"
                if os.path.lexists(backup):
                    raise ValueError("An unjournaled installer backup already exists")
                with old.open("rb") as source:
                    write_file(backup, source.read, 0o755)
                record = {"identity": identity, "phase": "prepared",
                          "oldInstaller": current["installedNix"],
                          "newInstaller": manifest["installerSha256"],
                          "backup": backup.name, "keyPresent": current["keyPresent"],
                          "key": manifest["keySha256"],
                          "helper": f"/nix/store/{manifest['helper']}/bin/brook-stage-profile"}
                save(nix / BOOTSTRAP, record)
            except BaseException:
                remove_own_lock(nix, identity)
                raise
        installer_bytes, key_bytes = import_runtime(nix, manifest, archive, current["missing"])
        if not current["keyPresent"]:
            write_file(house / "stage-plan.pub", io.BytesIO(key_bytes).read, 0o644)
        target = nix / record["helper"].removeprefix("/nix/")
        if not aliases(nix):
            env = dict(os.environ)
            if fake:
                env["BROOK_STAGE_ROOT"] = str(nix.parent)
            subprocess.run([target, "initialize"], check=True, env=env)
        if not aliases(nix) or not link_equals(nix / PROFILE_ALIAS, LEGACY):
            raise ValueError("Profile initialize returned an unexpected layout")
        helper = nix / "bin/brook-stage-profile"
        if not helper.is_symlink():
            helper.symlink_to(record["helper"])
            fsync_dir(helper.parent)
        write_file(nix / "bin/nix-install", io.BytesIO(installer_bytes).read, 0o755)
        record["phase"] = "complete"
        save(nix / BOOTSTRAP, record)
        remove_own_lock(nix, identity)
        os.sync()
        return probe(nix, manifest, identity)
    finally:
        mutex.rmdir()


def rollback(nix, identity):
    saved = read_record(nix)
    if not saved or saved["identity"] != identity:
        raise ValueError("Bootstrap rollback record does not match")
    if saved["phase"] == "rolled_back":
        return {"phase": "rolled_back", "kind": "legacy"}
    house = nix / "etc/brook"
    mutex = house / ".profile-stage-bootstrap-lock"
    mutex.mkdir(mode=0o700)
    lock = nix / "var/.brook-profile-lock"
    locked = False
    try:
        lock.mkdir(mode=0o700)
        locked = True
        home = nix / PROFILE_ALIAS.rsplit("/", 1)[0]
        if (home / "current").is_symlink() and not link_equals(home / "current", LEGACY):
            raise ValueError("A later profile generation is active")
        profile = nix / "profile"
        apps = nix / "share/applications"
        old_apps = apps.with_name(".brook-applications-legacy")
        if profile.is_symlink() and not link_equals(profile, PROFILE_ALIAS):
            raise ValueError("Profile alias changed")
        if apps.is_symlink() and not link_equals(apps, APP_ALIAS):
            raise ValueError("Launcher alias changed")
        installer = nix / "bin/nix-install"
        backup = house / saved["backup"]
        trusted_file(backup, executable=True)
        if digest(backup) != saved["oldInstaller"] or digest(installer) not in (
                saved["oldInstaller"], saved["newInstaller"]):
            raise ValueError("Nix installer or backup changed")
        helper = nix / "bin/brook-stage-profile"
        key = house / "stage-plan.pub"
        if os.path.lexists(helper) and not link_equals(helper, saved["helper"]):
            raise ValueError("Stage helper changed")
        if os.path.lexists(key) and digest(key) != saved["key"]:
            raise ValueError("Stage plan key changed")
        if (home / LEGACY).is_dir():
            if profile.is_symlink():
                profile.unlink()
            if profile.exists():
                raise ValueError("Legacy profile destination is occupied")
            os.rename(home / LEGACY, profile)
            fsync_dir(nix)
        if old_apps.is_dir():
            if apps.is_symlink():
                apps.unlink()
            if apps.exists():
                raise ValueError("Legacy launcher destination is occupied")
            os.rename(old_apps, apps)
            fsync_dir(apps.parent)
        (home / "current").unlink(missing_ok=True)
        helper.unlink(missing_ok=True)
        if not saved["keyPresent"]:
            key.unlink(missing_ok=True)
        with backup.open("rb") as source:
            write_file(installer, source.read, 0o755)
        remove_own_lock(nix, identity)
        saved["phase"] = "rolled_back"
        save(nix / BOOTSTRAP, saved)
        os.sync()
        return {"phase": "rolled_back", "kind": "legacy"}
    finally:
        if locked:
            lock.rmdir()
        mutex.rmdir()


def main():
    root = os.environ.get("BROOK_STAGE_ROOT")
    if not root and os.geteuid() != 0:
        raise ValueError("Bootstrap requires a maintenance operator")
    nix = Path(root).resolve() / "nix" if root else Path("/nix")
    action, identity, *rest = sys.argv[1:]
    if len(identity) != 64 or any(c not in "0123456789abcdef" for c in identity):
        raise ValueError("Expected a bootstrap SHA-256 identity")
    if action == "rollback":
        state = rollback(nix, identity)
    else:
        path = Path(rest[0])
        if digest(path) != identity:
            raise ValueError("Bootstrap manifest identity mismatch")
        manifest = manifest_data(path)
        if action == "check":
            state = probe(nix, manifest, identity)
        elif action == "apply":
            state = apply(nix, manifest, identity, Path(rest[1]), json.loads(rest[2]), bool(root))
        else:
            raise ValueError(f"Unknown bootstrap action {action!r}")
    print("STAGE_BOOTSTRAP " + json.dumps(state, sort_keys=True), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        sys.exit(f"stage-profile bootstrap: {error}")

"""Signed profile staging for the persistent USB desktop."""

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import stat
import sys
import tempfile

# The boot launcher supplies the pinned support directory and the expected
# identity of the installed selected-profile runtime.
support, helper, helper_hash = sys.argv[1:4]
sys.path.append(support)
import stage_profile as stage
from stage_nar import PackedCache, check_legacy_links, ensure_path, rename_no_replace, verify_installed
from stage_selected_space import check_capacity, selected_capacity, selected_transaction, writable_tree
import shutil

NIX = Path("/nix")
BOOT_KEY = Path("/etc/usb-profile-plan.pub")
DISK_KEY = NIX / "etc/brook/stage-plan.pub"
BOOT_ID = Path("/data/.brook-profile-boot")
HOME = NIX / stage.PROFILE_HOME
LOCK = NIX / "var/.brook-profile-lock"
STATE = HOME / ".stage"
FLOOR = STATE / "usb-high-water.json"
RESERVE = 1 << 30
ID = re.compile("[0-9a-f]{32}")
SCRATCH = re.compile(r"\.brook-(?:nar|profile-building)-[a-z0-9_]{8}")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def boot_id():
    result = BOOT_ID.read_text().strip()
    if not ID.fullmatch(result):
        raise ValueError("Invalid USB boot identity")
    return result


def root_owned(path, directory):
    info = path.lstat()
    kind = stat.S_ISDIR if directory else stat.S_ISREG
    if not kind(info.st_mode) or info.st_uid or info.st_mode & 0o022:
        raise ValueError(f"Untrusted USB staging path: {path}")


def runtime_identity():
    installed = NIX / "bin/brook-stage-profile"
    if installed.resolve(strict=True) != Path(helper) or digest(Path(helper)) != helper_hash:
        raise ValueError("Unexpected installed selected-profile helper")
    if stage.cache_capabilities.__module__ != "stage_profile" or not hasattr(stage, "validate_usage"):
        raise ValueError("Unsupported selected-profile runtime")


def verified(document, signature):
    root_owned(DISK_KEY, False)
    if digest(DISK_KEY) != digest(BOOT_KEY):
        raise ValueError("USB profile signing key mismatch")
    plan, fingerprint = stage.verified_plan(document, signature, BOOT_KEY, os.environ["BROOK_STAGE_OPENSSL"])
    number = plan.get("sequence")
    if plan["format"] != 3 or type(number) is not int or not 0 < number < (1 << 63):
        raise ValueError("A positive signed USB profile sequence is required")
    return plan, fingerprint


def check_sequence(plan, fingerprint, transaction, *, active=None):
    if not FLOOR.exists():
        return
    state = json.loads(FLOOR.read_text())
    if plan["sequence"] > state["sequence"]:
        return
    same = [plan["sequence"], fingerprint, transaction] == [
        state["sequence"], state["digest"], state["transaction"]]
    if same and active == ".brook-profile-" + transaction:
        return
    raise ValueError("USB profile sequence was already used or is below the high-water mark")


@contextlib.contextmanager
def locked():
    with tempfile.TemporaryDirectory(prefix=".brook-lock-building-", dir=LOCK.parent) as temporary:
        candidate = Path(temporary)
        stage.durable_text(candidate / "owner", json.dumps({"boot": boot_id(), "pid": os.getpid()}) + "\n")
        rename_no_replace(candidate, LOCK)
        stage.flush_directory(LOCK.parent)
        try:
            yield
        finally:
            (LOCK / "owner").unlink(missing_ok=True)
            LOCK.rmdir()
            stage.flush_directory(LOCK.parent)


def save_floor(plan, fingerprint, transaction):
    fd, filename = tempfile.mkstemp(prefix=".usb-high-water-", dir=STATE)
    path = Path(filename)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump({"sequence": plan["sequence"], "digest": fingerprint,
                       "transaction": transaction}, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(path, FLOOR)
        stage.flush_directory(STATE)
    finally:
        path.unlink(missing_ok=True)


def remove_tree(path):
    if path.is_dir() and not path.is_symlink():
        writable_tree(path)
        shutil.rmtree(path)
    else:
        path.unlink(missing_ok=True)


def lock_owner():
    if not stat.S_ISDIR(LOCK.lstat().st_mode):
        raise ValueError("Invalid USB staging lock")
    path = LOCK / "owner"
    if not stat.S_ISREG(path.lstat().st_mode):
        raise ValueError("Invalid USB staging lock owner")
    return json.loads(path.read_text())


def job_active():
    if not os.path.lexists(LOCK):
        return False
    owner = lock_owner()
    if owner.get("boot") != boot_id():
        return False
    pid = owner.get("pid")
    if type(pid) is not int or pid <= 0:
        raise ValueError("Invalid USB staging process identity")
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass
    return True


def recover():
    owner_path = LOCK / "owner"
    owner = lock_owner()
    previous = owner.get("boot")
    if not isinstance(previous, str) or not ID.fullmatch(previous) or previous == boot_id():
        raise ValueError("Recovery requires a profile lock from an earlier boot")
    for directory in (NIX / "store", HOME):
        for path in directory.iterdir():
            if SCRATCH.fullmatch(path.name) and path.is_dir() and not path.is_symlink():
                if (path / "old").exists() or (path / "old").is_symlink():
                    name = stage.safe_name((path / "repair-name").read_text())
                    target = directory / name
                    if not os.path.lexists(target):
                        rename_no_replace(path / "old", target)
                remove_tree(path)
        stage.flush_directory(directory)
    owner_path.unlink()
    LOCK.rmdir()
    stage.flush_directory(LOCK.parent)


def repair_path(store, name, entry, plan, tools, cache):
    # Keep the original in the same filesystem until replacement is durable.
    with tempfile.TemporaryDirectory(prefix=".brook-nar-", dir=store) as temporary:
        scratch = Path(temporary)
        ensure_path(scratch, name, entry, plan["cache"], tools, packed_cache=cache)
        stage.durable_text(scratch / "repair-name", name)
        old, target = scratch / "old", store / name
        rename_no_replace(target, old)
        try:
            stage.flush_directory(scratch)
            stage.flush_directory(store)
            rename_no_replace(scratch / name, target)
            stage.flush_directory(store)
        except BaseException:
            if not os.path.lexists(target):
                rename_no_replace(old, target)
            raise
        finally:
            if os.path.lexists(target):
                remove_tree(old)


def apply(plan, fingerprint, transaction, bundles, repairs):
    target = ".brook-profile-" + transaction
    STATE.mkdir(mode=0o700, exist_ok=True)
    record = STATE / (transaction + ".json")
    store = NIX / "store"
    tools = {name: str(NIX / "bin" / name) for name in ("curl", "xz", "zstd", "nar-unpack")}
    with locked(), selected_transaction(record) as published, PackedCache(bundles) as cache:
        previous = stage.active_generation(NIX)
        check_sequence(plan, fingerprint, transaction, active=previous)
        if previous == target and record.is_file():
            return
        if os.path.lexists(HOME / target) or record.exists():
            raise ValueError("An unactivated USB generation requires a new transaction")
        _, overlay = stage.collect_user_overlay(NIX, HOME / previous)
        capacity = selected_capacity(NIX, HOME, len(overlay))
        present = {name for name in plan["closure"] if os.path.lexists(store / name)}
        missing_or_damaged = set(repairs)
        if not missing_or_damaged <= plan["closure"].keys():
            raise ValueError("Repair requested outside the signed closure")
        check_capacity(plan, capacity, present - missing_or_damaged, transfer_bytes=RESERVE)
        legacy, counts = set(), {}
        for name, entry in sorted(plan["closure"].items()):
            if name in missing_or_damaged and name in present:
                try:
                    verify_installed(store / name, entry, legacy)
                except (ValueError, OSError):
                    if not cache.contains(name):
                        raise ValueError(f"Repair requires the signed archive for {name}") from None
                    repair_path(store, name, entry, plan, tools, cache)
                    counts["repaired"] = counts.get("repaired", 0) + 1
                    continue
            try:
                result = ensure_path(store, name, entry, plan["cache"], tools,
                                     legacy_paths=legacy, packed_cache=cache)
            except ValueError:
                if name in present:
                    print("USB_DAMAGED_STORE_PATH " + name, file=sys.stderr, flush=True)
                raise
            counts[result] = counts.get(result, 0) + 1
        check_legacy_links(store, plan["closure"], legacy)
        print(json.dumps({"closure": counts}), flush=True)
        temp = Path(tempfile.mkdtemp(prefix=".brook-profile-building-", dir=HOME))
        try:
            stage.prepare(NIX, plan, previous, temp, legacy)
            temp.chmod(0o755)
            original = temp.lstat()
            rename_no_replace(temp, HOME / target)
            published(HOME / target, original)
            stage.flush_directory(HOME)
        finally:
            if temp.exists():
                remove_tree(temp)
        stage.durable_text(record, json.dumps({"previous": previous, "digest": fingerprint}) + "\n")
        # Persist the floor before any activation, including across rollback.
        save_floor(plan, fingerprint, transaction)
        stage.switch(HOME, target)
    stage.notify(NIX)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("initialize", "apply", "verify", "rollback",
                                           "recover", "cache-capabilities", "status"))
    parser.add_argument("transaction", nargs="?")
    parser.add_argument("plan", nargs="?", type=Path)
    parser.add_argument("signature", nargs="?", type=Path)
    parser.add_argument("--cache-bundle", type=Path, action="append", default=[])
    parser.add_argument("--repair", action="append", default=[])
    args = parser.parse_args(sys.argv[4:])
    expected_uid = 0 if args.action in ("initialize", "recover") else stage.DESKTOP_UID
    if os.geteuid() != expected_uid:
        raise ValueError(f"{args.action} requires UID {expected_uid}")
    if args.action == "status":
        print(json.dumps({"active": job_active()}))
    elif args.action == "cache-capabilities":
        result = stage.cache_capabilities(NIX)
        result.update(usb_profile_api=1, plan_key_sha256=digest(BOOT_KEY),
                      guest_key_sha256=digest(DISK_KEY))
        print(json.dumps(result))
    elif args.action == "initialize":
        runtime_identity()
        root_owned(NIX / "etc", True)
        (NIX / "etc/brook").mkdir(mode=0o755, exist_ok=True)
        root_owned(NIX / "etc/brook", True)
        if os.path.lexists(NIX / "profile/.mutation-lock") or os.path.lexists(LOCK):
            raise ValueError("Another installer currently owns the profile")
        if not os.path.lexists(DISK_KEY):
            fd, filename = tempfile.mkstemp(prefix=".usb-plan-key-", dir=DISK_KEY.parent)
            path = Path(filename)
            try:
                with os.fdopen(fd, "wb") as stream:
                    stream.write(BOOT_KEY.read_bytes())
                    stream.flush()
                    os.fchmod(stream.fileno(), 0o644)
                    os.fsync(stream.fileno())
                os.link(path, DISK_KEY)
                stage.flush_directory(DISK_KEY.parent)
            finally:
                path.unlink(missing_ok=True)
        root_owned(DISK_KEY, False)
        if digest(DISK_KEY) != digest(BOOT_KEY):
            raise ValueError("USB profile signing key mismatch")
        stage.initialize(NIX)
    elif args.action == "recover":
        recover()
    elif not args.transaction or not ID.fullmatch(args.transaction):
        raise ValueError("Expected a USB profile transaction ID")
    elif args.action == "rollback":
        with locked():
            previous = json.loads((STATE / (args.transaction + ".json")).read_text())["previous"]
            current = stage.active_generation(NIX)
            if current != previous:
                if current != ".brook-profile-" + args.transaction or not stage.GENERATION.fullmatch(previous):
                    raise ValueError("Rollback does not match the current USB profile")
                stage.switch(HOME, previous)
        stage.notify(NIX)
    else:
        plan, fingerprint = verified(args.plan, args.signature)
        if args.action == "verify":
            check_sequence(plan, fingerprint, args.transaction)
        else:
            apply(plan, fingerprint, args.transaction, args.cache_bundle, args.repair)
    if args.action not in ("status", "cache-capabilities"):
        print("PROFILE_" + args.action.upper() + "_OK " + (args.transaction or ""), flush=True)


def interrupted(_signal, _frame):
    raise ValueError("USB profile job interrupted")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        sys.exit(f"USB profile: {error}")

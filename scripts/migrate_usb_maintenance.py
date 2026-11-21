#!/usr/bin/env python3
"""Back up, migrate, or restore the mounted FAT ESP of a USB maintenance stick.

Use a private host directory for the state and keep it: the byte image and
receipt may contain credentials. Confirm the device with the operator first.
Mount only the ESP read-write with a normal vfat mixed/winnt short-name
policy. Close other applications and keep the stick attached. Capture reads the
whole USB disk to a private stick.img and verifies the SHA-256 on readback.
check verifies the backup and free capacity. apply stages the reviewed files,
moves all old top-level entries into OLD, then publishes the new EFI loader
last. Firmware cannot boot the old tree from OLD.

If interrupted or reversing the migration, use restore with the same arguments
and reviewed files. It verifies all original content before moving it back and
declines to remove unknown contents. Keep stick.img for offline recovery if
filesystem damage prevents normal restoration. The backup and OLD can contain
the old shared key, so keep both private.
"""
import argparse
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import tempfile

from bridge_keyless_image import require_keyless_disk, require_keyless_image
from bridge_operator_policy import validate_operator_policy

NAMES = ("KERNEL/BROOK.ELF", "INITRD.IMG", "EFI/BOOT/BOOTX64.EFI")
LIMITS = (8 << 20, 64 << 20, 4 << 20)
FORMAT = "brook-usb-maintenance-v3"
STAGE = "BRKNEW"
ARCHIVE = "OLD"
BUFFER = 1 << 20
BLKGETSIZE64 = 0x80081272


def private_dir(path):
    meta = path.lstat()
    if not stat.S_ISDIR(meta.st_mode) or meta.st_uid != os.geteuid() or meta.st_mode & 0o077:
        raise ValueError("The state directory must be a private host directory owned by the caller")


def safe_open(path, flags=os.O_RDONLY, mode=0o600):
    return os.open(path, flags | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, mode)


def read_regular(path, limit):
    fd = safe_open(path)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
            raise ValueError(f"{path}: expected a bounded regular file")
        value = stream.read(limit + 1)
        if len(value) != info.st_size:
            raise ValueError(f"{path}: size changed during read")
        return value


def digest_stream(stream, output=None):
    digest = hashlib.sha256()
    size = 0
    while chunk := stream.read(BUFFER):
        size += len(chunk)
        digest.update(chunk)
        if output is not None:
            output.write(chunk)
    return {"bytes": size, "sha256": digest.hexdigest()}


def digest_file(path, *, private=False):
    with os.fdopen(safe_open(path), "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or (private and (
                stat.S_IMODE(info.st_mode) & 0o077 or info.st_uid != os.geteuid())):
            raise ValueError(f"{path}: expected a regular {'private ' if private else ''}file")
        result = digest_stream(stream)
        if result["bytes"] != info.st_size:
            raise ValueError(f"{path}: size changed during read")
        return result


def sync_directory(path):
    fd = safe_open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def sync_volume(path):
    fd = safe_open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        if ctypes.CDLL(None, use_errno=True).syncfs(fd):
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), path)
    finally:
        os.close(fd)


def write_file(path, data):
    fd = safe_open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def store_record(path, value):
    with tempfile.TemporaryDirectory(dir=path.parent, prefix=".receipt-") as temporary:
        receipt_path = Path(temporary) / "record"
        write_file(receipt_path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode())
        os.link(receipt_path, path)
    sync_directory(path.parent)


def unescape_mount(value):
    return re.sub(r"\\([0-7]{3})", lambda group: chr(int(group[1], 8)), value)


def usb_mount(mount, disk):
    """Independently bind the mounted FAT partition to the named USB whole disk."""
    mount = mount.absolute()
    if mount.resolve(strict=True) != mount:
        raise ValueError("Use the exact mountpoint without symlinks or '..'")
    mounts = []
    other_mounted = set()
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        prefix, suffix = line.split(" - ", 1)
        info = prefix.split()
        target = Path(unescape_mount(info[4]))
        if target == mount:
            mounts.append((info, suffix.split()))
        elif mount in target.parents:
            raise ValueError("The ESP has a nested mount")
        else:
            other_mounted.add(info[2])
    if len(mounts) != 1:
        raise ValueError("Use an independently mounted FAT partition")
    info, fs = mounts[0]
    if info[3] != "/" or fs[0] != "vfat":
        raise ValueError("Mount must expose the whole FAT filesystem using vfat")
    if {"shortname=lower", "shortname=win95"} & set(fs[2].split(",")):
        raise ValueError("Mount with shortname=mixed or shortname=winnt to preserve FAT names")
    if "rw" not in info[5].split(","):
        raise ValueError("Mount the FAT filesystem read-write for capture, check and migration")
    number = os.makedev(*map(int, info[2].split(":")))
    if mount.stat().st_dev != number:
        raise ValueError("FAT mount identity changed")
    sys_part = (Path("/sys/dev/block") / info[2]).resolve(strict=True)
    if not (sys_part / "partition").is_file():
        raise ValueError("Mount must be a partition on the USB disk")
    sys_disk = sys_part.parent
    for mounted in other_mounted:
        mounted_device = (Path("/sys/dev/block") / mounted).resolve()
        if mounted_device == sys_disk or sys_disk in mounted_device.parents:
            raise ValueError("The USB disk is mounted elsewhere. Unmount the other USB volumes first")
    if not any((parent / "subsystem").resolve().name == "usb" for parent in sys_disk.parents):
        raise ValueError("The source disk is not attached via USB")
    disk = disk.resolve(strict=True)
    dev = disk.stat()
    if not stat.S_ISBLK(dev.st_mode):
        raise ValueError("Specify the full USB block device")
    named = Path(f"/sys/dev/block/{os.major(dev.st_rdev)}:{os.minor(dev.st_rdev)}")
    if named.resolve(strict=True) != sys_disk:
        raise ValueError("The mounted partition does not belong to the named USB disk")
    part_start = int((sys_part / "start").read_text()) * 512
    part_bytes = int((sys_part / "size").read_text()) * 512
    with os.fdopen(safe_open(disk), "rb") as source:
        if os.fstat(source.fileno()).st_rdev != dev.st_rdev:
            raise ValueError("The USB block device changed")
        size = struct.unpack("=Q", fcntl.ioctl(source.fileno(), BLKGETSIZE64, bytes(8)))[0]
        source.seek(part_start)
        boot_sector = source.read(512)
    if len(boot_sector) != 512 or not (0 < part_start < part_start + part_bytes <= size):
        raise ValueError("Invalid USB partition geometry")
    return {"disk": str(disk), "disk_device": dev.st_rdev,
            "sysfs": str(sys_disk), "disk_bytes": size,
            "fat_device": number, "fat_start": part_start, "fat_bytes": part_bytes,
            "fat_boot_sector_sha256": hashlib.sha256(boot_sector).hexdigest()}


def raw_disk(identity, output=None):
    with os.fdopen(safe_open(identity["disk"]), "rb") as stream:
        if os.fstat(stream.fileno()).st_rdev != identity["disk_device"]:
            raise ValueError("The USB disk changed")
        result = digest_stream(stream, output)
    if result["bytes"] != identity["disk_bytes"]:
        raise ValueError("The raw read did not cover the full USB disk")
    return result


def inventory(root):
    device = root.stat().st_dev
    result = {}

    def walk(directory):
        folded = set()
        for child in sorted(directory.iterdir()):
            key = child.name.casefold()
            info = child.lstat()
            if key in folded or info.st_dev != device:
                raise ValueError("Ambiguous FAT name or nested filesystem")
            folded.add(key)
            name = child.relative_to(root).as_posix()
            if stat.S_ISDIR(info.st_mode):
                result[name] = None
                walk(child)
            elif stat.S_ISREG(info.st_mode):
                result[name] = digest_file(child)
            else:
                raise ValueError(f"{name}: expected only files and directories on FAT")

    walk(root)
    return result


def entries(tree):
    return [name for name in tree if "/" not in name]


def reject_reserved(tree):
    if {name.casefold() for name in entries(tree)} & {ARCHIVE.lower(), STAGE.lower()}:
        raise ValueError("The FAT root already contains OLD or BRKNEW")


def file_set(payload):
    manifest = json.loads(read_regular(payload / "manifest.json", BUFFER))
    if manifest.get("format") != FORMAT or set(manifest.get("files", {})) != set(NAMES):
        raise ValueError("Unexpected maintenance file-set format or paths")
    options = manifest.get("kernel_options", {})
    if (options.get("BROOK_BRIDGE_V3") != "ON" or
            any(options.get(option) != "OFF" for option in (
                "BROOK_QEMU_LEGACY_OUTBOUND_DEBUG", "BROOK_SSD_GUARD_CHECK",
                "BROOK_SSD_AUTO_RECOVERY", "BROOK_SSD_GUARDED_DESKTOP", "BROOK_SSD_INSTALLER"))):
        raise ValueError("Expected the standalone v3 maintenance build mode")
    policy = manifest.get("bridge_v3", {})
    if policy.get("version") != 3 or not re.fullmatch("[0-9a-f]{64}", policy.get("policy_sha256", "")):
        raise ValueError("The file set has no v3 operator policy")
    tree = inventory(payload)
    parent_paths = {str(parent) for name in NAMES for parent in Path(name).parents if parent != Path(".")}
    if set(tree) != set(NAMES) | parent_paths | {"manifest.json"}:
        raise ValueError("Unexpected file or directory in the maintenance file set")
    result = {}
    for name, limit in zip(NAMES, LIMITS, strict=True):
        value = read_regular(payload / name, limit)
        expected = {"bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()}
        if not value or expected != manifest["files"][name]:
            raise ValueError(f"{name}: maintenance file hash or size mismatch")
        result[name] = value
    if not result[NAMES[0]].startswith(b"\x7fELF") or not result[NAMES[2]].startswith(b"MZ"):
        raise ValueError("Expected an ELF kernel and a PE executable")
    # Check exactly the bytes retained for apply. The input directory may be
    # provisioned by a separate build or copied from another host.
    with tempfile.TemporaryDirectory(prefix="brook-usb-payload-") as directory:
        for index, name in enumerate(NAMES):
            path = Path(directory) / str(index)
            write_file(path, result[name])
            if name == "INITRD.IMG":
                require_keyless_image(path)
                embedded = subprocess.check_output(["mtype", "-i", path, "::/ETC/bridge-v3-operators"])
                validate_operator_policy(embedded)
                listing = subprocess.check_output(["mdir", "-a", "-b", "-i", path, "::/ETC"], text=True)
                unavailable = {"::/ETC/" + entry for entry in (
                    "ahci-data-volume", "boot-app-image", "boot-app-snapshot",
                    "writable-desktop", "usb-boot-poweroff")}
                if unavailable.intersection(listing.splitlines()):
                    raise ValueError("The maintenance image enables desktop or USB reset options")
                if hashlib.sha256(embedded).hexdigest() != policy["policy_sha256"]:
                    raise ValueError("The immutable maintenance bridge policy differs from the manifest")
            else:
                require_keyless_disk(path)
    return result


def ensure_capacity(mount, old, payload):
    fs = os.statvfs(mount)
    cluster = fs.f_frsize
    if cluster < 512:
        raise ValueError("Invalid FAT allocation block size")

    def clusters(size):
        return (size + cluster - 1) // cluster

    def fat_slots(name):
        # Reserve both the long-name entries and its short alias, even when
        # the filesystem can omit the long name for an 8.3 spelling.
        units = len(name.encode("utf-16le")) // 2
        return (units + 12) // 13 + 1

    archive_slots = 2 + sum(fat_slots(name) for name in entries(old))
    data_blocks = sum(clusters(len(value)) for value in payload.values())
    # BRKNEW, EFI, BOOT, KERNEL and OLD directory space plus root growth.
    directories = 4 + clusters(32 * archive_slots) + 2
    have = fs.f_bavail
    required = data_blocks + directories
    if have < required:
        raise ValueError(f"FAT has room for {have * cluster} additional bytes, needs {required * cluster}")
    return {"free_bytes": have * cluster, "required_additional_bytes": required * cluster}


def ensure_host_state(path, identity):
    if path.resolve(strict=True) != path:
        raise ValueError("Use a host backup directory without symlinks or '..'")
    dev = path.stat().st_dev
    local = (Path("/sys/dev/block") / f"{os.major(dev)}:{os.minor(dev)}").resolve()
    stick = Path(identity["sysfs"])
    if dev == identity["fat_device"] or local == stick or stick in local.parents:
        raise ValueError("The backup must be on a private host filesystem outside the USB disk")


def capture(mount, disk, state):
    identity = usb_mount(mount, disk)
    ensure_host_state(state.parent, identity)
    state.mkdir(mode=0o700)
    sync_volume(mount)
    original = inventory(mount)
    reject_reserved(original)
    backup = state / "stick.img"
    with os.fdopen(safe_open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL), "wb") as output:
        copied = raw_disk(identity, output)
        output.flush()
        os.fsync(output.fileno())
    if (digest_file(backup, private=True) != copied or usb_mount(mount, disk) != identity or
            inventory(mount) != original):
        raise ValueError("The backup readback or mounted source changed")
    store_record(state / "backup.json", {"source": identity, "image": copied, "original": original})
    sync_directory(state.parent)
    return {"backup": copied, "old_top_level_count": len(entries(original))}


def receipt(mount, disk, state):
    private_dir(state)
    identity = usb_mount(mount, disk)
    ensure_host_state(state, identity)
    record = json.loads(read_regular(state / "backup.json", 64 << 20))
    if identity != record["source"]:
        raise ValueError("The mounted USB disk or partition no longer matches the backup")
    if digest_file(state / "stick.img", private=True) != record["image"]:
        raise ValueError("Full private backup size or SHA-256 mismatch")
    return record


def preflight(mount, disk, state, payload):
    record = receipt(mount, disk, state)
    if (state / "apply.json").exists():
        raise ValueError("There is a pending or completed migration. Use restore first")
    old = inventory(mount)
    reject_reserved(old)
    if old != record["original"] or raw_disk(record["source"]) != record["image"]:
        raise ValueError("The current USB bytes or FAT tree differ from the private backup")
    return old, ensure_capacity(mount, old, payload)


def stage_tree(payload):
    tree = {}
    for name, value in payload.items():
        for parent in Path(name).parents:
            if parent != Path("."):
                tree[str(parent)] = None
        tree[name] = {"bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()}
    return tree


def migrate(mount, state, old, payload):
    expected = stage_tree(payload)
    if inventory(mount) != old:
        raise ValueError("The FAT contents changed since preflight")
    store_record(state / "apply.json", {"replacement": expected})
    fresh, archive = mount / STAGE, mount / ARCHIVE
    fresh.mkdir()
    for name, value in payload.items():
        target = fresh / name
        target.parent.mkdir(parents=True, exist_ok=True)
        write_file(target, value)
    if inventory(fresh) != expected:
        raise ValueError("Staged maintenance file readback mismatch")
    sync_volume(mount)
    archive.mkdir()
    for name in sorted(entries(old), key=lambda item: item.casefold() != "efi"):
        target = archive / name
        if target.exists():
            raise ValueError("Unexpected archived path already exists")
        (mount / name).rename(target)
    sync_volume(mount)
    if inventory(archive) != old:
        raise ValueError("Archived FAT tree readback mismatch")
    store_record(state / "archived.json", {"published": True})
    for name in ("KERNEL", "INITRD.IMG", "EFI"):
        if (mount / name).exists():
            raise ValueError("Unexpected file at a maintenance boot location")
        (fresh / name).rename(mount / name)
        sync_volume(mount)
    fresh.rmdir()
    sync_volume(mount)
    installed = {name: item for name, item in inventory(mount).items()
                 if name != ARCHIVE and not name.startswith(ARCHIVE + "/")}
    if installed != expected or inventory(archive) != old:
        raise ValueError("Final FAT readback mismatch")
    return {"new_files": expected, "old_top_level_count": len(entries(old))}


def subtree(tree, parent):
    return {name: value for name, value in tree.items()
            if name == parent or name.startswith(parent + "/")}


def removable(tree, root, payload, expected, *, partial):
    for name, observed in tree.items():
        if name not in expected:
            raise ValueError(f"Unexpected file while restoring: {name}")
        if observed == expected[name]:
            continue
        if (not partial or observed is None or expected[name] is None or
                observed["bytes"] > expected[name]["bytes"] or
                read_regular(root / name, expected[name]["bytes"]) !=
                payload[name][:observed["bytes"]]):
            raise ValueError(f"Modified file while restoring: {name}")


def remove_tree(root, tree):
    for name, info in reversed(sorted(tree.items())):
        (root / name).rmdir() if info is None else (root / name).unlink()


def restore(mount, state, original, payload):
    expected = stage_tree(payload)
    transaction = json.loads(read_regular(state / "apply.json", BUFFER))
    if transaction != {"replacement": expected}:
        raise ValueError("Supply the same reviewed replacement files to restore")
    before = inventory(mount)
    fresh, archive = mount / STAGE, mount / ARCHIVE
    if any(name.casefold() in (STAGE.lower(), ARCHIVE.lower()) and
           (name not in (STAGE, ARCHIVE) or before[name] is not None) for name in entries(before)):
        raise ValueError("Unexpected transaction path on FAT")
    stashed = inventory(archive) if ARCHIVE in before else {}
    staging = inventory(fresh) if STAGE in before else {}
    if {name: original.get(name) for name in stashed} != stashed or set(stashed) - set(original):
        raise ValueError("The archived original files changed")
    removable(staging, fresh, payload, expected, partial=True)
    old_names = entries(original)
    for name in entries(stashed):
        if subtree(stashed, name) != subtree(original, name):
            raise ValueError(f"Incomplete archived original: {name}")
    for name in old_names:
        if name not in stashed and subtree(before, name) != subtree(original, name):
            raise ValueError(f"Missing or modified original FAT content: {name}")
    remaining = {name: value for name, value in before.items() if
                 name.split("/", 1)[0] not in (ARCHIVE, STAGE) and
                 (name.split("/", 1)[0] not in old_names or name.split("/", 1)[0] in stashed)}
    archived = (state / "archived.json").exists()
    restoring = (state / "restoring.json").exists()
    if remaining and not archived:
        raise ValueError("Unexpected installed files before archive completion")
    removable(remaining, mount, payload, expected, partial=False)
    if archived and not restoring:
        for name in old_names:
            if name not in stashed:
                raise ValueError(f"Original content escaped the verified archive: {name}")
    if not restoring:
        store_record(state / "restoring.json", {"started": True})
    remove_tree(mount, remaining)
    remove_tree(fresh, staging)
    if STAGE in before:
        fresh.rmdir()
    sync_volume(mount)
    # Restore EFI last. Firmware must see all the old supporting files first.
    for name in sorted(entries(stashed), key=lambda item: item.casefold() == "efi"):
        (archive / name).rename(mount / name)
        sync_volume(mount)
    if ARCHIVE in before:
        archive.rmdir()
    sync_volume(mount)
    if inventory(mount) != original:
        raise ValueError("Restored FAT readback mismatch")
    (state / "apply.json").unlink()
    for name in ("archived.json", "restoring.json"):
        (state / name).unlink(missing_ok=True)
    sync_directory(state)
    return {"restored_top_level_count": len(old_names)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("capture", "check", "apply", "restore"))
    parser.add_argument("--mount", type=Path, required=True, help="exact ESP mountpoint")
    parser.add_argument("--disk", type=Path, required=True, help="confirmed whole USB block device")
    parser.add_argument("--state", type=Path, required=True, help="private off-stick backup directory")
    parser.add_argument("--files", type=Path, help="reviewed file set, required after capture")
    args = parser.parse_args()
    mount, state = args.mount.absolute(), args.state.absolute()
    try:
        if args.action == "capture":
            value = capture(mount, args.disk, state)
        else:
            if args.files is None:
                parser.error("--files is required for check, apply and restore")
            private_dir(state)
            # Serialize actions against the same transaction on this host.
            lock = safe_open(state, os.O_RDONLY | os.O_DIRECTORY)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                payload = file_set(args.files)
                if args.action == "restore":
                    record = receipt(mount, args.disk, state)
                    value = restore(mount, state, record["original"], payload)
                else:
                    old, capacity = preflight(mount, args.disk, state, payload)
                    value = capacity if args.action == "check" else migrate(mount, state, old, payload)
            finally:
                os.close(lock)
        print(json.dumps(value, indent=2, sort_keys=True))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()

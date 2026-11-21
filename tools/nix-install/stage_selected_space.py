"""Capacity accounting and owned-path cleanup for selected profile staging."""

import contextlib
import os
from pathlib import Path
import shutil
import stat
import sys
import uuid

from stage_nar import STORE_NAME, rename_no_replace

LAYOUT_FIELDS = ("links", "directories", "externalLinks", "externalLinkBytes", "directoryBytes")
MAX_INVENTORY_BYTES = 48 * 1024
# ext2 stores symlinks shorter than 60 bytes inside their inode.
INLINE_LINK_BYTES = 59


def layout_usage(layout):
    targets = [os.fsencode(value) for value in layout["links"].values()]
    external = [target for target in targets if len(target) > INLINE_LINK_BYTES]
    names = [*layout["directories"], *layout["links"]]
    return {
        "links": len(targets),
        "directories": len(layout["directories"]),
        "externalLinks": len(external),
        "externalLinkBytes": sum(map(len, external)),
        # ext2 directory entries have an eight-byte header and pad to four bytes.
        "directoryBytes": sum((11 + len(os.fsencode(Path(name).name))) & ~3 for name in names),
    }


def tree_inodes(path):
    count = 0
    pending = [path]
    while pending:
        node = pending.pop()
        count += 1
        if stat.S_ISDIR(os.lstat(node).st_mode):
            pending.extend(child.path for child in os.scandir(node))
    return count


def validate_usage(plan):
    usage = plan["profile"].get("space")
    if (not isinstance(usage, dict) or set(usage) != set(LAYOUT_FIELDS)
            or any(type(value) is not int or not 0 <= value <= (64 << 30)
                   for value in usage.values())
            or usage["externalLinks"] > usage["links"]):
        raise ValueError("Missing or invalid signed selected profile capacity recipe; regenerate the plan")
    for name, entry in plan["closure"].items():
        inodes = entry.get("storeInodes")
        if type(inodes) is not int or not 1 <= inodes <= entry["narSize"]:
            raise ValueError(f"Missing or invalid signed inode count for {name}; regenerate the plan")


def filesystem_capacity(path):
    space = os.statvfs(path)
    unit = space.f_frsize or space.f_bsize
    return {"block_bytes": unit, "free_blocks": space.f_bavail,
            "free_bytes": unit * space.f_bavail, "free_inodes": space.f_favail}


def selected_capacity(nix, home, overlay_links, transfer=None):
    store = filesystem_capacity(nix / "store")
    generations = filesystem_capacity(home)
    # Account against the scarcer view if these paths are mounted separately.
    # On Dell both views share one ext2 volume.
    if store["block_bytes"] != generations["block_bytes"]:
        raise ValueError("Selected store and profile block sizes differ")
    for field in ("free_bytes", "free_blocks", "free_inodes"):
        store[field] = min(store[field], generations[field])
    store["overlay_links"] = overlay_links
    if transfer is not None:
        store["transfer_shares_space"] = any(
            os.stat(transfer).st_dev == os.stat(path).st_dev for path in (nix / "store", home)
        ) or bool(
            getattr(os.statvfs(transfer), "f_fsid", 0)
            and os.statvfs(transfer).f_fsid in (
                getattr(os.statvfs(nix / "store"), "f_fsid", None),
                getattr(os.statvfs(home), "f_fsid", None),
            )
        )
        # Stay below the debug job's 64 KiB output ring. Unknown paths are
        # counted as missing by the host; the device checks exact paths again.
        names, used = [], 0
        for name in sorted(os.listdir(nix / "store")):
            if not STORE_NAME.fullmatch(name):
                continue
            used += len(name) + 4
            if used > MAX_INVENTORY_BYTES:
                break
            names.append(name)
        store["store_names"] = names
        store["store_inventory_complete"] = used <= MAX_INVENTORY_BYTES
    return store


def check_capacity(plan, capacity, names, *, generation=True, transfer_bytes=0, transfer_inodes=0):
    validate_usage(plan)
    if not isinstance(capacity, dict):
        raise ValueError("Selected helper lacks capacity reporting; refresh it first")
    values = [capacity.get(field) for field in ("block_bytes", "free_blocks", "free_bytes",
                                                "free_inodes", "overlay_links")]
    if any(type(value) is not int or value < 0 for value in values):
        raise ValueError("Invalid selected store capacity response")
    unit, free_blocks, free_bytes, free_inodes, overlay = values
    if not unit or free_blocks * unit != free_bytes:
        raise ValueError("Invalid selected store block capacity")

    def rounded(size):
        return ((size + unit - 1) // unit) * unit

    missing = [entry for name, entry in plan["closure"].items() if name not in names]
    inodes = sum(entry["storeInodes"] for entry in missing)
    # NAR payload, rounded files and directories, and one extraction scratch
    # peak. Packed compressed input can also be spooled during extraction.
    largest = max((entry["narSize"] for entry in missing), default=0)
    bytes_needed = sum(rounded(entry["narSize"]) + unit * entry["storeInodes"]
                       for entry in missing) + 2 * rounded(largest)
    if missing:
        inodes += 4
    if generation:
        shape = plan["profile"]["space"]
        inodes += shape["links"] + shape["directories"] + overlay
        bytes_needed += (
            unit * (shape["externalLinks"] + shape["directories"] + overlay)
            + rounded(shape["externalLinkBytes"] + shape["directoryBytes"])
        )
        # Gallery export parents, launcher index/icons, manifest and journal.
        extra = 16 + 3 * (overlay + len(plan["profile"]["launcherIds"]))
        inodes += extra
        bytes_needed += unit * extra
    # Account for indirect blocks, large icons and concurrent unrelated writes.
    if bytes_needed or inodes:
        bytes_needed = rounded(bytes_needed + max(16 << 20, bytes_needed // 16))
        inodes += max(64, inodes // 8)
    bytes_needed += transfer_bytes
    inodes += transfer_inodes
    if (bytes_needed > free_bytes or inodes > free_inodes):
        raise ValueError(
            f"Selected profile needs {bytes_needed} bytes ({bytes_needed // unit} blocks) "
            f"and {inodes} inodes on /nix; {free_bytes} bytes ({free_blocks} blocks) "
            f"and {free_inodes} inodes available"
        )
    return {"bytes": bytes_needed, "blocks": bytes_needed // unit,
            "inodes": inodes, "missing_store_paths": len(missing)}


def host_preflight(plan, capabilities, transfers):
    capacity = capabilities.get("selected_store")
    if not isinstance(capacity, dict):
        raise ValueError("Selected helper lacks capacity reporting; refresh it first")
    names = capacity.get("store_names")
    shares = capacity.get("transfer_shares_space")
    if (not isinstance(names, list)
            or any(not isinstance(name, str) or not STORE_NAME.fullmatch(name) for name in names)
            or type(shares) is not bool):
        raise ValueError("Invalid selected store inventory")
    transfer_bytes = transfer_inodes = 0
    if shares:
        unit = capacity.get("block_bytes")
        if type(unit) is not int or unit <= 0:
            raise ValueError("Invalid selected store block capacity")
        for path in transfers:
            transfer_bytes += ((path.stat().st_size + unit - 1) // unit) * unit
            transfer_inodes += 1
    return check_capacity(plan, capacity, set(names), transfer_bytes=transfer_bytes,
                          transfer_inodes=transfer_inodes)


def identity(st):
    return st.st_dev, st.st_ino, stat.S_IFMT(st.st_mode)


def writable_tree(path):
    # NAR directories can be read-only. This runs only after removing the
    # exact owned inode from its public name. Symlinks are never followed.
    for name, _, _ in os.walk(path, followlinks=False):
        os.chmod(name, stat.S_IMODE(os.lstat(name).st_mode) | stat.S_IWUSR)


def discard_owned(path, original):
    try:
        if identity(path.lstat()) != identity(original):
            return
    except FileNotFoundError:
        return
    tombstone = path.with_name(f".brook-abandoned-{uuid.uuid4().hex}")
    rename_no_replace(path, tombstone)
    if identity(tombstone.lstat()) != identity(original):
        # A replacement arrived before the rename. Keep it intact even
        # when its old name has already been taken by another caller.
        rename_no_replace(tombstone, path)
        return
    if stat.S_ISDIR(original.st_mode):
        writable_tree(tombstone)
        shutil.rmtree(tombstone)
    else:
        tombstone.unlink()


@contextlib.contextmanager
def selected_transaction(record):
    imported = []

    def published(path, original):
        imported.append((path, original))

    try:
        yield published
    except Exception:
        # A published journal can be retried or may already be active.
        # Keep its store paths and candidate if durability was ambiguous.
        if not os.path.lexists(record):
            for path, original in reversed(imported):
                try:
                    discard_owned(path, original)
                except OSError as error:
                    print(f"Selected profile cleanup could not remove {path.name}: {error}",
                          file=sys.stderr)
        raise

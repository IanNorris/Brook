"""Create the per-user Steam installation from the verified immutable seed."""
import os
from pathlib import Path
import shutil
import stat
import tempfile

from bootstrap import publish


# A seed may be split across store paths so that each one fits a bounded
# transfer. The file lists top-level entries that are symlinks to the other
# parts; they are copied as real directories, never kept as store links.
LAYERS = ".brook-seed-layers"


def seed_layers(seed):
    path = seed / LAYERS
    if not path.is_file():
        return set()
    names = set(path.read_text().split())
    for name in names:
        if "/" in name or name in (".", "..") or not (seed / name).is_symlink() or not (seed / name).is_dir():
            raise ValueError(f"Invalid Steam client seed layer: {name}")
    return names


def copy_tree(source, target, layers=frozenset()):
    target.mkdir(mode=0o700)
    for entry in source.iterdir():
        target_entry = target / entry.name
        if entry.name in layers:
            copy_tree(entry.resolve(), target_entry)
            continue
        if layers and entry.name == LAYERS:
            continue
        mode = entry.lstat().st_mode
        if stat.S_ISLNK(mode):
            target_entry.symlink_to(os.readlink(entry))
        elif stat.S_ISDIR(mode):
            copy_tree(entry, target_entry)
        elif stat.S_ISREG(mode):
            # copy2/copystat would use xattrs, which Brook's ext2 does not have.
            shutil.copyfile(entry, target_entry)
            target_entry.chmod(0o700 if mode & 0o111 else 0o600)
        else:
            raise ValueError(f"Unexpected file kind in the Steam client seed: {entry}")


def ensure_client(seed, install, version):
    """Publish a complete first install. Retain owned client and user state on updates."""
    client = install / "client"
    install.mkdir(mode=0o700, parents=True, exist_ok=True)
    state = install.lstat()
    if (not stat.S_ISDIR(state.st_mode) or state.st_uid != os.geteuid() or
            state.st_mode & 0o077):
        raise ValueError("Steam requires a private installation directory owned by the desktop user")
    if client.exists() or client.is_symlink():
        return installed_version(client)
    if not (seed / "steamrt64/steam").is_file() or not version.isdecimal():
        raise ValueError("The packaged Steam client seed is incomplete")
    # SteamRT provisions a mutable runtime. Keep its space separate from this
    # private copy of the read-only, versioned profile payload.
    layers = seed_layers(seed)
    seed_bytes = sum(path.stat().st_size
                     for root in (seed, *(seed / name for name in sorted(layers)))
                     for path in root.resolve().rglob("*")
                     if not path.is_symlink() and path.is_file())
    space = os.statvfs(install)
    if seed_bytes + 1024 ** 3 > space.f_bavail * space.f_frsize:
        raise ValueError("Steam requires enough home space for the pinned client plus 1 GiB of runtime workspace")
    with tempfile.TemporaryDirectory(prefix=".client-", dir=install) as directory:
        candidate = Path(directory) / "complete"
        copy_tree(seed, candidate, layers)
        (candidate / ".brook-seed-version").write_text(version + "\n")
        try:
            publish(candidate, client)
        except FileExistsError:
            # Another launcher may have published a complete installation first.
            return installed_version(client)
    return version


def installed_version(client):
    info = client.lstat()
    marker = client / ".brook-seed-version"
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or marker.is_symlink():
        raise ValueError("Unexpected existing Steam installation. Leave it in place and use a separate path")
    if not marker.is_file():
        raise ValueError("Existing Steam installation has no Brook package receipt. Its contents were preserved")
    version = marker.read_text().strip()
    if not version.isdecimal():
        raise ValueError("Existing Steam installation has an invalid Brook package receipt")
    return version

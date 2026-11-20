"""Keep desktop-user launch commands and metadata across profile generations."""

import os
import stat
from pathlib import Path

INDEX = "applications.idx"
MAX_INDEX = 64 * 1024
UID_PREFIX = "--uid 1000 "


def read_index(path):
    if not path.is_file():
        return []
    with path.open("rb") as stream:
        raw = stream.read(MAX_INDEX + 1)
    if len(raw) > MAX_INDEX or b"\0" in raw:
        raise ValueError(f"Invalid launcher index {path}")
    rows = []
    for line in raw.decode("utf-8").split("\n"):
        fields = line.removesuffix("\r").split("\t")
        if (len(fields) == 5 and fields[0] and fields[1] and fields[4]
                and len(fields[4].encode("utf-8")) < 128):
            rows.append(fields)
    return rows


def executable(path, nix, profile):
    if path.startswith("-"):
        return False
    if "/" not in path:
        if path in (".", ".."):
            return False
        return any(executable(root + path, nix, profile)
                   for root in ("/nix/profile/bin/", "/nix/bin/"))
    if not path.startswith("/") or any(part in ("", ".", "..") for part in path[1:].split("/")):
        return False
    if path.startswith("/nix/profile/bin/"):
        candidate = profile / path.removeprefix("/nix/profile/")
    elif path.startswith(("/nix/bin/", "/nix/store/", "/data/", "/boot/")) or (
            path.startswith("/nix/") and path.count("/") == 2 and path.endswith(".sh")):
        candidate = nix.parent / path[1:]
    else:
        return False
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        return False
    if path.startswith("/nix/store/"):
        try:
            return candidate.samefile(profile / "bin" / candidate.name)
        except OSError:
            return False
    return True


def desktop_command(command, nix, profile):
    # The kernel only recognises double quotes at a token boundary. Carry
    # simple words, then pin the sole run option before the validated program.
    if not command.isascii() or any(ord(c) < 32 or ord(c) == 127 or c in "\"'\\" for c in command):
        return None
    program = command.removeprefix(UID_PREFIX)
    words = program.split()
    if not words or any(word.startswith("#") for word in words):
        return None
    result = UID_PREFIX + program
    if len(result) >= 128 or len(words) > 29:
        return None
    if not executable(words[0], nix, profile):
        return None
    if Path(words[0]).name == "brook-run":
        if len(words) < 2 or not executable(words[1], nix, profile):
            return None
    return result


def keep_icon(old, new, name):
    if not name:
        return ""
    if (not name.isascii() or not name.endswith(".rgba") or len(name) >= 128
            or name.startswith(".") or any(not c.isalnum() and c not in "._-" for c in name)):
        return ""
    source, target = old / "icons" / name, new / "icons" / name
    if target.is_file():
        return name
    try:
        info = source.lstat()
    except FileNotFoundError:
        return ""
    if not stat.S_ISREG(info.st_mode) or info.st_size != 24 * 24 * 4:
        return ""
    with source.open("rb") as stream:
        data = stream.read(24 * 24 * 4 + 1)
    if len(data) != 24 * 24 * 4:
        return ""
    target.write_bytes(data)
    return name


def merge_indexes(nix, profile, old, new):
    planned = {row[4]: row for row in read_index(new / INDEX)}
    titles = {row[0].casefold() for row in planned.values()}
    published = set()
    rows = []
    for old_row in read_index(old / INDEX):
        title, command, icon, categories, app_id = old_row
        if app_id in published:
            continue
        safe = desktop_command(command, nix, profile)
        if app_id in planned:
            row = planned[app_id].copy()
            words = command.removeprefix(UID_PREFIX).split()
            curated = command.startswith(UID_PREFIX) or (words and Path(words[0]).name == "brook-run")
            if curated:
                protected = safe or desktop_command(row[1], nix, profile)
                if not protected:
                    raise ValueError(f"No eligible desktop-user command for curated application {app_id}")
                row[1] = protected
            if not row[2]:
                row[2] = keep_icon(old, new, icon)
        elif safe and title.casefold() not in titles:
            row = [title, safe, keep_icon(old, new, icon), categories, app_id]
            titles.add(title.casefold())
        else:
            continue
        published.add(app_id)
        rows.append(row)
    rows.extend(row for app_id, row in planned.items() if app_id not in published)
    output = "".join("\t".join(row) + "\n" for row in rows).encode("utf-8")
    if (len(output) > MAX_INDEX or any(len(line) > 1024 for line in output.split(b"\n"))
            or any(len(row[1].encode("utf-8")) >= 128 for row in rows)):
        raise ValueError("Merged launcher index exceeds the desktop limits")
    (new / INDEX).write_bytes(output)

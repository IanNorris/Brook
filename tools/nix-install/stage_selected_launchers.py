"""Merge a signed app selection with Gallery-owned launcher entries."""

from pathlib import Path
import re

from stage_launchers import INDEX, MAX_INDEX, UID_PREFIX, desktop_command, keep_icon

APP_ID = re.compile(r"[A-Za-z0-9_.-]{1,127}\Z")


def validate_launcher_ids(ids):
    if (not isinstance(ids, list) or len(ids) > 128
            or any(not isinstance(value, str) or not APP_ID.fullmatch(value) for value in ids)
            or len(ids) != len(set(ids))):
        raise ValueError("Invalid selected profile launcher IDs")


def read_index(path):
    if not path.is_file():
        return []
    with path.open("rb") as stream:
        raw = stream.read(MAX_INDEX + 1)
    if len(raw) > MAX_INDEX or b"\0" in raw:
        raise ValueError(f"Invalid launcher index {path}")
    rows = []
    for line in raw.decode("utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) in (5, 6) and fields[0] and fields[1] and APP_ID.fullmatch(fields[4]):
            rows.append(fields)
    return rows


def curated(command):
    words = command.split()
    return command.startswith(UID_PREFIX) or (words and Path(words[0]).name == "brook-run")


def merge_selected_indexes(nix, profile, old, new, launcher_ids, overlay):
    # The signed layout admits the managed entries. Gallery ownership comes
    # from the independently validated relative desktop exports in the old
    # generation, never from an old launcher row's executable alone.
    planned = read_index(new / INDEX)
    allowed = set(launcher_ids)
    if len(planned) != len(allowed) or {row[4] for row in planned} != allowed:
        raise ValueError("Managed desktop entries differ from the signed selection")
    gallery = {path.stem for path in overlay
               if path.parent == Path("share/applications") and path.suffix == ".desktop"}
    former = {}
    for old_row in read_index(old / INDEX):
        former.setdefault(old_row[4], old_row)

    rows = []
    titles = {row[0].casefold() for row in planned}
    for row in planned:
        app_id = row[4]
        previous = former.get(app_id)
        current_command = desktop_command(row[1], nix, profile)
        if not current_command:
            raise ValueError(f"No eligible desktop-user command for selected application {app_id}")

        if previous and (app_id in gallery or curated(previous[1])):
            # Existing curated commands may still be preferred, but only if
            # their program (and a brook-run payload) survives in this profile.
            prior_command = desktop_command(previous[1], nix, profile)
            if prior_command:
                current_command = prior_command
                if app_id in gallery and (previous[0].casefold() == row[0].casefold()
                                          or previous[0].casefold() not in titles):
                    original_icon = row[2]
                    row = previous.copy()
                    row[2] = keep_icon(old, new, row[2]) or original_icon
                    titles.add(row[0].casefold())
            if not row[2]:
                row[2] = keep_icon(old, new, previous[2])
        if previous and len(previous) == 6 and len(row) == 5:
            row.append(previous[5])
        row[1] = current_command
        rows.append(row)

    for app_id, row in former.items():
        title = row[0].casefold()
        if app_id not in gallery or app_id in allowed or title in titles:
            continue
        command = desktop_command(row[1], nix, profile)
        if command:
            row[1] = command
            row[2] = keep_icon(old, new, row[2])
            rows.append(row)
            titles.add(title)

    output = "".join("\t".join(row) + "\n" for row in rows).encode("utf-8")
    if len(output) > MAX_INDEX or any(len(line) > 1024 for line in output.split(b"\n")):
        raise ValueError("Selected launcher index exceeds the desktop limits")
    (new / INDEX).write_bytes(output)

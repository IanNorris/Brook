"""Retention for journaled, inactive profile generations."""

import json
import os
import re
import shutil

from stage_nar import rename_no_replace

GENERATION = re.compile(r"[.]brook-profile-(?:legacy|[0-9a-f]{32})")
LEGACY = ".brook-profile-legacy"
PREFIX = ".brook-profile-"


def real_directory(path):
    return path.is_dir() and not path.is_symlink()


def journal_previous(home, generation):
    if generation == LEGACY:
        return None
    transaction = generation.removeprefix(PREFIX)
    record = home / ".stage" / (transaction + ".json")
    if not record.is_file() or record.is_symlink():
        raise ValueError(f"Missing stage journal for {generation}")
    document = json.loads(record.read_text())
    previous = document.get("previous") if isinstance(document, dict) else None
    if not isinstance(previous, str) or not GENERATION.fullmatch(previous):
        raise ValueError(f"Invalid stage journal for {generation}")
    return previous


def pin_generation(home, transaction, pinned, flush_directory):
    if not isinstance(transaction, str) or not re.fullmatch(r"[0-9a-f]{32}", transaction):
        raise ValueError("Invalid profile transaction")
    name = PREFIX + transaction
    if not real_directory(home / name):
        raise ValueError(f"Generation is unavailable: {name}")
    pins = home / ".pins"
    pins.mkdir(mode=0o700, exist_ok=True)
    if not real_directory(pins):
        raise ValueError("Invalid profile pins directory")
    marker = pins / name
    if os.path.lexists(marker) and (not marker.is_file() or marker.is_symlink()):
        raise ValueError(f"Invalid pin for {name}")
    if pinned:
        marker.touch(exist_ok=True)
    else:
        marker.unlink(missing_ok=True)
    flush_directory(pins)
    flush_directory(home)


def prune_generations(home, current, flush_directory, *, dry_run=False):
    if not real_directory(home / current):
        raise ValueError("Active profile generation is unavailable")
    previous = journal_previous(home, current)
    if previous == current:
        raise ValueError("Circular profile generation history")
    # Follow only the recorded ancestry. A prepared sibling or a generation
    # left by a rollback is still available for retry, and is left alone.
    lineage = {current, previous, LEGACY}
    stale = []
    candidate = journal_previous(home, previous) if previous else None
    while candidate and candidate != LEGACY:
        if candidate in lineage:
            raise ValueError("Circular profile generation history")
        lineage.add(candidate)
        stale.append(candidate)
        candidate = journal_previous(home, candidate)

    pins = home / ".pins"
    if os.path.lexists(pins) and not real_directory(pins):
        raise ValueError("Invalid profile pins directory")
    removed = []
    for name in stale:
        if os.path.lexists(pins / name):
            continue
        path = home / name
        retired = home / (".brook-pruned-" + name.removeprefix(PREFIX))
        if os.path.lexists(path) and os.path.lexists(retired):
            raise ValueError(f"Conflicting pruned generation {name}")
        if not os.path.lexists(path) and not os.path.lexists(retired):
            continue
        if not real_directory(path if os.path.lexists(path) else retired):
            raise ValueError(f"Invalid generation directory {name}")
        if not dry_run:
            if os.path.lexists(path):
                rename_no_replace(path, retired)
                flush_directory(home)
            shutil.rmtree(retired)
            flush_directory(home)
        removed.append(name)
    return {"current": current, "previous": previous, "generations": removed,
            "dry_run": dry_run}

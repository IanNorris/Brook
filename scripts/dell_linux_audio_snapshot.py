#!/usr/bin/env python3
"""Collect narrow, read-only Linux reference data for Dell XPS 9343 audio.

Run on a Linux boot of the same machine. No device files or physical memory
are opened. Missing devices and inaccessible logs are recorded explicitly.
"""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import subprocess

IDS = ("INT3438", "INT33C8", "INT343A", "INT33CA", "INT3439", "INT33C9")
MESSAGES = re.compile(r"catpt|\bsst\b|rt28[68]|i2c[-_]designware|snd[-_]soc|sof[-_]audio", re.I)
LIMIT = 4 * 1024 * 1024


def read(path, limit=65536):
    try:
        with path.open("rb") as source:
            data = source.read(limit + 1)
        if len(data) > limit:
            return {"error": "size limit exceeded"}
        return {"text": data.decode("utf-8", errors="replace").strip()}
    except OSError as exc:
        return {"error": exc.strerror or type(exc).__name__}


def link_name(path):
    try:
        return os.path.basename(os.readlink(path).rstrip("/"))
    except OSError:
        return None


def enumerate_devices(root, relative, extra_names=(), extra_drivers=()):
    directory = root / relative
    try:
        entries = sorted(directory.iterdir(), key=lambda p: p.name)
    except OSError as exc:
        return {"error": exc.strerror or type(exc).__name__, "devices": []}
    result = []
    for entry in entries:
        driver = link_name(entry / "driver")
        if not (any(token in entry.name.upper() for token in IDS) or
                entry.name.lower() in extra_names or driver in extra_drivers):
            continue
        record = {"name": entry.name, "driver": driver}
        for field in ("status", "modalias", "hid", "uid", "resource", "irq"):
            record[field] = read(entry / field)
        record["physical_nodes"] = []
        for node in sorted(entry.glob("physical_node*")):
            item = {"link": node.name, "device": link_name(node), "driver": link_name(node / "driver")}
            for field in ("modalias", "resource", "irq"):
                item[field] = read(node / field)
            record["physical_nodes"].append(item)
        result.append(record)
    return {"devices": result}


def enumerate_sound_cards(root):
    directory = root / "sys/class/sound"
    try:
        entries = sorted(directory.iterdir(), key=lambda p: p.name)
    except OSError as exc:
        return {"error": exc.strerror or type(exc).__name__, "cards": []}
    cards = []
    for entry in entries:
        if not re.fullmatch(r"card\d+", entry.name):
            continue
        cards.append({"name": entry.name,
                      "id": read(entry / "id"),
                      "device": link_name(entry / "device"),
                      "driver": link_name(entry / "device/driver")})
    return {"cards": cards}


def snapshot(root=Path("/"), dmesg=None):
    result = {
        "schema": 1,
        "kernel_release": read(root / "proc/sys/kernel/osrelease"),
        "acpi": enumerate_devices(root, "sys/bus/acpi/devices"),
        "platform": enumerate_devices(root, "sys/bus/platform/devices",
                                      extra_names=("broadwell-audio",),
                                      extra_drivers=("broadwell-audio",)),
        "i2c": enumerate_devices(root, "sys/bus/i2c/devices", extra_drivers=("rt286",)),
        "sound_cards": read(root / "proc/asound/cards"),
        "sound_pcm": read(root / "proc/asound/pcm"),
        "sound_card_devices": enumerate_sound_cards(root),
        "firmware_tables": {},
        "firmware": {},
    }
    tables = root / "sys/firmware/acpi/tables"
    try:
        table_paths = sorted(p for p in tables.iterdir() if p.name == "DSDT" or re.fullmatch(r"SSDT\d*", p.name))
    except OSError:
        table_paths = []
        result["firmware_tables_error"] = "unavailable"
    for path in table_paths:
        try:
            with path.open("rb") as source:
                data = source.read(LIMIT + 1)
            if len(data) > LIMIT:
                result["firmware_tables"][path.name] = {"error": "size limit exceeded"}
            else:
                result["firmware_tables"][path.name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        except OSError as exc:
            result["firmware_tables"][path.name] = {"error": exc.strerror}
    for name in ("intel/IntcSST2.bin", "intel/catpt/bdw/dsp_basefw.bin"):
        path = next((root / "lib/firmware" / (name + suffix) for suffix in ("", ".xz", ".zst")
                     if (root / "lib/firmware" / (name + suffix)).exists()), None)
        if path is None:
            result["firmware"][name] = {"error": "not found"}
            continue
        try:
            with path.open("rb") as source:
                data = source.read(LIMIT + 1)
            if len(data) > LIMIT:
                result["firmware"][name] = {"error": "stored size limit exceeded"}
                continue
            record = {"stored_name": path.name, "stored_size": len(data), "stored_sha256": hashlib.sha256(data).hexdigest()}
            if path.suffix == ".xz":
                decoder = lzma.LZMADecompressor(memlimit=32 * 1024 * 1024)
                payload = decoder.decompress(data, max_length=LIMIT + 1)
                if len(payload) > LIMIT or not decoder.eof or decoder.unused_data:
                    record["payload_error"] = "incomplete, oversized or trailing compressed stream"
                    payload = None
            elif path.suffix == ".zst":
                # A bounded external decoder may not exist on a minimal live image.
                # Record the stored hash without pretending it is the payload hash.
                payload = None
                record["payload_error"] = "zstd payload not decoded"
            else:
                payload = data
            if payload is not None:
                record["payload_size"] = len(payload)
                record["payload_sha256"] = hashlib.sha256(payload).hexdigest()
            result["firmware"][name] = record
        except (OSError, lzma.LZMAError) as exc:
            result["firmware"][name] = {"error": str(exc)}
    selected = None if dmesg is None else [line for line in dmesg.splitlines() if MESSAGES.search(line)]
    result["kernel_messages"] = None if selected is None else selected[:200]
    result["kernel_messages_truncated"] = False if selected is None else len(selected) > 200
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New JSON file; never overwrites")
    args = parser.parse_args()
    try:
        run = subprocess.run(["dmesg", "--color=never"], text=True, capture_output=True, timeout=10, check=False)
        messages = run.stdout if run.returncode == 0 else None
        error = None if run.returncode == 0 else "dmesg unavailable"
    except (OSError, subprocess.TimeoutExpired):
        messages, error = None, "dmesg unavailable"
    result = snapshot(dmesg=messages)
    if error:
        result["kernel_messages_error"] = error
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2, sort_keys=True)
        output.write("\n")
    print(f"Audio reference saved to {args.output}")


if __name__ == "__main__":
    main()

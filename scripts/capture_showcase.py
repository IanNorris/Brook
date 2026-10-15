#!/usr/bin/env python3
"""Capture a genuine native-resolution Brook frame, plus provenance metadata."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "vm_bridge"))
import server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", help="VM bridge session ID; never the host desktop")
    parser.add_argument("name", help="Artifact basename, e.g. brook-quake2-gl3-1080")
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "showcase")
    parser.add_argument("--caption", required=True, help="Describe the observed app and state")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", args.name):
        parser.error("name must contain only lowercase letters, digits, hyphens or underscores")
    args.output.mkdir(parents=True, exist_ok=True)
    image_path = args.output / (args.name + ".png")
    metadata_path = image_path.with_suffix(".json")
    if image_path.exists() or metadata_path.exists():
        parser.error("Artifacts already exist; choose a new name or output directory")
    result = server.observe(args.session)  # Deliberately no crop or max_width.
    frame = result["data"]
    actual = frame["coordinate_width"], frame["coordinate_height"]
    pixels = frame["width"], frame["height"]
    expected = args.width, args.height
    if actual != expected or pixels != expected or frame["crop_origin"] != [0, 0]:
        raise RuntimeError(f"Native capture {actual}, image {pixels}; expected {expected}. No upscale allowed.")
    if frame.get("panic"):
        raise RuntimeError("Guest has panicked; this cannot be published as a working-app screenshot")
    guest_state = {}
    for action in ("windows", "processes"):
        try:
            guest_state[action] = server.guest_inspect(args.session, action=action)
        except (OSError, ValueError, RuntimeError) as error:
            guest_state[action] = {"error": str(error)}
    image_path.write_bytes(result["image"])
    metadata = {
        "caption": args.caption,
        "frame": frame,
        "sha256": hashlib.sha256(result["image"]).hexdigest(),
        "session": args.session,
        "native_pixels": list(pixels),
        "scaling": "none",
        "provenance": server.load_session(args.session),
        "guest_state": guest_state,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps({"image": str(image_path), "metadata": str(metadata_path), "pixels": pixels}))


if __name__ == "__main__":
    main()

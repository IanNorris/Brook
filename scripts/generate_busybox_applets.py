#!/usr/bin/env python3
"""Regenerate the kernel's fallback allowlist from the shipped BusyBox binary."""
from pathlib import Path
import argparse
import json
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "src/kernel/src/busybox_applets.h")
    args = parser.parse_args()
    names = sorted(set(subprocess.check_output([str(args.binary.resolve()), "--list"], text=True).splitlines()))
    if not names or any(not name or "/" in name or "\0" in name for name in names):
        parser.error("BusyBox did not return valid applet names")
    body = """#pragma once
// Applets in the static BusyBox installed by fetch_userspace.sh.
// Regenerate with: python3 scripts/generate_busybox_applets.py ./busybox_static
// Missing commands must stay ENOENT so execvp can continue searching PATH.
inline bool BusyboxHasApplet(const char* name)
{
    static constexpr const char names[] =
"""
    body += "".join("        " + json.dumps(name)[:-1] + '\\0"\n' for name in names)
    body += """        ;
    for (const char* entry = names; *entry;) {
        const char* a = entry;
        const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) return true;
        while (*entry) ++entry;
        ++entry;
    }
    return false;
}
"""
    args.output.write_text(body)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""waygap — Wayland API-gap auditor for Brook (P0.1 + P0.2).

Motivated by BRO-216: waylandd never advertised zwp_text_input_manager_v3, and
SDL3 silently disabled text input. libwayland-server cannot observe a global a
client wanted but was never offered, so the only cheap defense is DIFFERENTIAL:
compare what an app BINDS on a known-good reference (real Linux compositor)
against what it can bind on Brook, and compare Brook's advertised set against a
required profile.

Three subcommands:

  binds   <wayland-debug.log>...     Extract the set of globals a client bound
                                     from WAYLAND_DEBUG=1 output (wl_registry
                                     .bind lines). Works for Brook OR Linux logs.

  advertised <serial.log>...         Extract the globals waylandd advertised
                                     (WAYLAND_GLOBAL interface=.. version=.. lines
                                     emitted by the P0.1 startup dump) + any
                                     WAYLAND_GAP MISSING/VERSION_LOW findings.

  diff --ref REF.binds --brook BROOK.binds
                                     Show globals the app bound on the reference
                                     but NOT on Brook = the BRO-216 class. Exit
                                     non-zero if any gap is found (for CI).

WAYLAND_DEBUG bind line format (both server and client emit it), e.g.:
  [1234567.890]  -> wl_registry@2.bind(1, "zwp_text_input_manager_v3", 1, new id ...)
  [1234567.890] wl_registry@2.bind(1, "wl_compositor", 4, new id wl_compositor@3)
We parse the quoted interface name and the version that follows it.
"""
import argparse
import re
import sys

# Matches a wl_registry .bind request in WAYLAND_DEBUG output, capturing the
# bound interface name and the version argument that follows it. Tolerant of the
# arrow prefix, whitespace, and the varying "new id" tail.
BIND_RE = re.compile(
    r'wl_registry@\d+\.bind\(\s*\d+\s*,\s*"([^"]+)"\s*,\s*(\d+)'
)

# P0.1 startup dump line: WAYLAND_GLOBAL interface=<name> version=<v>
ADV_RE = re.compile(r'WAYLAND_GLOBAL\s+interface=(\S+)\s+version=(\d+)')
GAP_RE = re.compile(r'WAYLAND_GAP\s+(MISSING|VERSION_LOW)\s+interface=(\S+)')


def _read_lines(paths):
    if not paths:
        yield from sys.stdin
        return
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as f:
            yield from f


def cmd_binds(args):
    """Print 'interface version' for each distinct global the client bound."""
    best = {}
    for line in _read_lines(args.logs):
        m = BIND_RE.search(line)
        if m:
            iface, ver = m.group(1), int(m.group(2))
            best[iface] = max(best.get(iface, 0), ver)
    for iface in sorted(best):
        print(f"{iface} {best[iface]}")
    return 0


def cmd_advertised(args):
    """Print 'interface version' for each global waylandd advertised, and echo
    any WAYLAND_GAP findings the startup self-check produced."""
    best = {}
    gaps = []
    for line in _read_lines(args.logs):
        m = ADV_RE.search(line)
        if m:
            iface, ver = m.group(1), int(m.group(2))
            best[iface] = max(best.get(iface, 0), ver)
        g = GAP_RE.search(line)
        if g:
            gaps.append((g.group(1), g.group(2)))
    for iface in sorted(best):
        print(f"{iface} {best[iface]}")
    for kind, iface in gaps:
        print(f"# GAP {kind} {iface}", file=sys.stderr)
    return 0


def _load_pairs(path):
    """Load 'interface version' lines into {interface: version}."""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            iface = parts[0]
            ver = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
            out[iface] = ver
    return out


def cmd_diff(args):
    """Report globals bound on the reference but missing/older on Brook."""
    ref = _load_pairs(args.ref)
    brook = _load_pairs(args.brook)

    missing = sorted(i for i in ref if i not in brook)
    lower = sorted(i for i in ref if i in brook and brook[i] < ref[i])

    if missing:
        print("MISSING on Brook (app bound it on reference, Brook never offered it):")
        for i in missing:
            print(f"  - {i} (ref v{ref[i]})")
    if lower:
        print("VERSION LOWER on Brook (potential latent degradation):")
        for i in lower:
            print(f"  ~ {i} (ref v{ref[i]}, brook v{brook[i]})")
    if not missing and not lower:
        print("OK: Brook offers every global this app bound on the reference.")
        return 0
    return 1


def main():
    ap = argparse.ArgumentParser(
        description="Wayland API-gap auditor for Brook (P0.1 + P0.2).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  # Reference (real Linux compositor):
  WAYLAND_DEBUG=1 <app> 2> ref.wl.log; waygap.py binds ref.wl.log > ref.binds
  # Brook (serial log from a headless run):
  waygap.py binds brook_serial.log > brook.binds
  # The BRO-216-class catch:
  waygap.py diff --ref ref.binds --brook brook.binds
  # Startup advertised set + self-check findings:
  waygap.py advertised brook_serial.log
""",
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_b = sub.add_parser("binds", help="extract bound globals from WAYLAND_DEBUG log")
    p_b.add_argument("logs", nargs="*", help="WAYLAND_DEBUG log files (default stdin)")
    p_b.set_defaults(func=cmd_binds)

    p_a = sub.add_parser("advertised", help="extract advertised globals from a Brook serial log")
    p_a.add_argument("logs", nargs="*", help="serial log files (default stdin)")
    p_a.set_defaults(func=cmd_advertised)

    p_d = sub.add_parser("diff", help="diff reference binds against Brook binds/advertised")
    p_d.add_argument("--ref", required=True, help="reference 'interface version' file")
    p_d.add_argument("--brook", required=True, help="Brook 'interface version' file")
    p_d.set_defaults(func=cmd_diff)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()

"""Counter-tier gap analysis for the P2 gate.

The deterministic backbone: turn the kernel's Ctrl+F11 gap dump (syscall-gap +
sub-gap tables) into a normalized set of gap *identities*, and diff a run
against a checked-in baseline. A gap's identity is (kind, number, sub) only --
hit counts, first_pid, and first_comm are diagnostic noise that must be
normalized away, or every run would "differ".

Fail conditions (a NEW gap not in baseline and not in the EXPECTED_GAP
allowlist): a previously-unseen unimplemented syscall, or an unsupported
subcommand/flag reaching the kernel. That is exactly the silent-degradation a
nixpkgs bump (moving GTK/Qt/Mesa onto a new syscall) would introduce.
"""
import json
import re
from pathlib import Path

_SYSCALL_GAP = re.compile(
    r"SYSCALL_GAP nr=(\d+) name=(\S+) hits=(\d+) .*?class=(\S+)")
_SUBGAP = re.compile(
    r"SUBGAP syscall=(\d+) sub=(0x[0-9a-fA-F]+|\d+) hits=(\d+)")


def parse_gaps(gap_lines):
    """Return {identity_key: {fields}} keyed by normalized identity."""
    gaps = {}
    for ln in gap_lines:
        m = _SYSCALL_GAP.search(ln)
        if m:
            nr, name, hits, cls = m.groups()
            key = f"syscall:nr={nr}"
            gaps[key] = {"kind": "syscall", "nr": int(nr), "name": name,
                         "class": cls, "hits": int(hits)}
            continue
        m = _SUBGAP.search(ln)
        if m:
            syscall, sub, hits = m.groups()
            sub_norm = sub if sub.startswith("0x") else hex(int(sub))
            key = f"sub:syscall={syscall}:sub={sub_norm}"
            gaps[key] = {"kind": "sub", "syscall": int(syscall),
                         "sub": sub_norm, "hits": int(hits)}
    return gaps


def load_baseline(path):
    p = Path(path)
    if not p.exists():
        return None
    data = json.loads(p.read_text())
    return data


def load_allowlist(path):
    """EXPECTED_GAP lines: `EXPECTED_GAP key=<identity> reason=...`.

    Returns a set of allowed identity keys. Missing file => empty set.
    """
    p = Path(path)
    allowed = set()
    if not p.exists():
        return allowed
    for ln in p.read_text().splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        m = re.search(r"key=(\S+)", ln)
        if m:
            allowed.add(m.group(1))
    return allowed


def compare(observed, baseline, allowlist):
    """Diff observed identities vs baseline+allowlist.

    Returns (ok, report) where report has new_gaps / missing_gaps / known.
    A NEW gap (present now, absent from baseline and allowlist) fails the gate.
    A MISSING gap (in baseline, gone now) is reported but does NOT fail -- it's
    usually a genuine improvement, surfaced for human review.
    """
    obs_keys = set(observed.keys())
    base_keys = set((baseline or {}).get("gaps", {}).keys())
    allowed = set(allowlist)

    new_gaps = sorted(obs_keys - base_keys - allowed)
    missing_gaps = sorted(base_keys - obs_keys)
    known = sorted(obs_keys & (base_keys | allowed))

    ok = len(new_gaps) == 0
    report = {
        "ok": ok,
        "new_gaps": [{"key": k, **observed[k]} for k in new_gaps],
        "missing_gaps": missing_gaps,
        "known_count": len(known),
        "have_baseline": baseline is not None,
    }
    return ok, report


def write_baseline(path, probe, observed):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(
        {"probe": probe, "gaps": observed}, indent=2, sort_keys=True))

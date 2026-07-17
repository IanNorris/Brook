# P2 regression gate

Sentinel-gated functional harness for the Brook API-gap audit. Boots Brook
headless under QEMU (software composite, single-CPU via BROOK_SMP=1), drives a
representative app into a target feature with deterministic HMP key injection,
and asserts a **semantic** outcome (a persisted application artifact) rather
than process survival.

## Design (see artifacts/brook-api-gap-audit-plan.md for the full rationale)

Three assertion tiers, weakest-sufficient per probe:
1. counter/manifest deltas (deterministic) - the backbone (WIP: counter tier)
2. logged semantic outcome - implemented (Slice 1)
3. golden pixel diff - last resort, software-composite only (WIP)

Hard rules baked in:
- QEMU owned via `-pidfile` + shut down with HMP `quit`; never a name-based
  process sweep (shared container).
- Every readiness wait is a serial sentinel with a hard-fail timeout; no
  fixed-sleep "act anyway" path.
- Single-CPU boot to dodge the intermittent BRO-208 SMP race.

## Usage

    python3 tools/p2-gate/p2gate.py --probe mousepad            # positive gate
    python3 tools/p2-gate/p2gate.py --probe mousepad --negative # self-test

The `--negative` self-test omits the typing; the gate MUST go red on the
semantic assertion. It proves the gate catches "typing does nothing" (the
BRO-216 class) rather than merely producing green reports. Exit 0 = the gate
correctly caught the induced regression.

## Slice 1: mousepad edit-and-save

Opens a seeded file, injects Ctrl+A / "Brook123" / Ctrl+S, and a guest-side
watcher emits a single-line marker once the saved bytes contain the magic.
Verified: positive PASS 3/3 (repeatable), negative self-test catches the
regression. `/tmp` is RAM-backed on Brook, so the oracle uses an in-guest
watcher (same VFS) rather than a post-quit disk read.

## Counter tier (deterministic backbone)

Each run parses the Ctrl+F11 gap dump (syscall-gap + sub-gap tables) into a set
of normalized gap *identities* -- `(kind, number, sub)` only; hit counts,
first_pid and first_comm are diagnostic noise that is normalized away. The run
is diffed against a blessed baseline plus an EXPECTED_GAP allowlist:

    baseline/<probe>.gaps.json       # blessed stable gap set
    baseline/<probe>.expected_gaps   # documented known/intermittent gaps

A NEW gap (present now, absent from both) FAILS the gate -- that is exactly the
silent degradation a nixpkgs bump (moving a toolkit onto a new syscall) would
introduce. A MISSING gap is reported but does not fail (usually an improvement).

    python3 tools/p2-gate/p2gate.py --probe mousepad --bless-counters

Bless refuses if any functional assertion is red (never bless a broken run).
Intermittent-but-benign gaps (e.g. glibc's inotify->polling fallback) belong in
the allowlist, not a single-run baseline, so they never flake the gate.

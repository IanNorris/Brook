#!/usr/bin/env python3
"""Validate overlapping client-scanout snapshots from one boot."""

import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics
import sys


IDENTITY = ("owner", "offer", "buffer", "worker", "admitted_us")
TIMES = ("admitted_us", "flush_start_us", "flush_end_us", "arm_us", "latch_us", "retired_us")
TERMINAL = ("replacement", "restore", "rejected")
FIELDS = set(IDENTITY + TIMES + ("sequence", "retired_by", "retirement", "latch_observer"))


def load_rows(capture):
    rows = {}
    last_sequence = 0
    snapshots = capture if isinstance(capture, list) else [capture]
    for snapshot in snapshots:
        reply = snapshot.get("reply", snapshot)
        columns = reply.get("columns", [])
        records = reply.get("rows", [])
        if (reply.get("clock") != "monotonic" or not FIELDS <= set(columns)
                or len(columns) != len(set(columns)) or reply.get("count") != len(records)):
            raise ValueError("Invalid or incompatible client timeline snapshot")
        if any(len(record) != len(columns) for record in records):
            raise ValueError("Client timeline row does not match its columns")
        parsed = [dict(zip(columns, record)) for record in records]
        sequences = [entry["sequence"] for entry in parsed]
        if (any(type(n) is not int or n <= 0 for n in sequences)
                or sorted(set(sequences)) != sequences
                or (last_sequence and not sequences)
                or (sequences and sequences[-1] < last_sequence)):
            raise ValueError("Sequence reset or reordered snapshots; use one boot per capture")
        for entry in parsed:
            sequence = entry["sequence"]
            if any(type(entry[key]) is not int or entry[key] < 0 for key in (*TIMES, "retired_by")):
                raise ValueError(f"Invalid time or successor at sequence {sequence}")
            prior = rows.get(sequence)
            if prior:
                if any(prior[key] != entry[key] for key in IDENTITY):
                    raise ValueError(f"Conflicting identity for sequence {sequence}")
                if any(prior[key] and prior[key] != entry[key] for key in (*TIMES, "retired_by")):
                    raise ValueError(f"Conflicting recorded event for sequence {sequence}")
                if prior["latch_us"] and prior["latch_observer"] != entry["latch_observer"]:
                    raise ValueError(f"Conflicting observer for sequence {sequence}")
                if prior["retired_us"] and prior["retirement"] != entry["retirement"]:
                    raise ValueError(f"Conflicting retirement for sequence {sequence}")
            rows[sequence] = entry
        if sequences:
            last_sequence = sequences[-1]
    return rows


def distribution(values):
    ordered = sorted(values)
    return {
        "count": len(ordered),
        "p50_us": statistics.median(ordered),
        "p90_us": ordered[math.ceil(0.9 * len(ordered)) - 1],
        "max_us": ordered[-1],
    }


def summarize(rows):
    errors, unresolved, joined = [], [], []
    latencies = defaultdict(list)
    for sequence, entry in sorted(rows.items()):
        times = [entry[key] for key in TIMES if entry[key]]
        if times != sorted(times):
            errors.append({"sequence": sequence, "reason": "event_time_order"})
        observer = entry["latch_observer"]
        if ((entry["latch_us"] and observer not in ("existing", "offworker"))
                or (not entry["latch_us"] and observer != "none")):
            errors.append({"sequence": sequence, "reason": "invalid_latch_observer"})
        if entry["latch_us"] and entry["arm_us"] and entry["admitted_us"]:
            latencies[(observer, "arm_to_latch")].append(entry["latch_us"] - entry["arm_us"])
            latencies[(observer, "admit_to_latch")].append(entry["latch_us"] - entry["admitted_us"])
        retirement = entry["retirement"]
        successor = entry["retired_by"]
        if retirement not in ("held", *TERMINAL) or bool(entry["retired_us"]) != (retirement in TERMINAL):
            errors.append({"sequence": sequence, "reason": "invalid_retirement"})
        if retirement != "replacement":
            if successor:
                errors.append({"sequence": sequence, "reason": "unexpected_successor"})
            continue
        if successor <= sequence:
            errors.append({"sequence": sequence, "reason": "invalid_successor", "successor": successor})
            continue
        next_entry = rows.get(successor)
        if not next_entry or not next_entry["latch_us"]:
            unresolved.append({"sequence": sequence, "successor": successor})
            continue
        delay = entry["retired_us"] - next_entry["latch_us"]
        if delay < 0:
            errors.append({"sequence": sequence, "reason": "retired_before_latch", "successor": successor})
        else:
            joined.append(delay)
    by_observer = {}
    for (observer, stage), values in sorted(latencies.items()):
        by_observer.setdefault(observer, {})[stage] = distribution(values)
    return {
        "unique_rows": len(rows),
        "sequence_gaps": max(rows) - min(rows) + 1 - len(rows) if rows else 0,
        "retirements": dict(Counter(entry["retirement"] for entry in rows.values())),
        "latency_by_observer": by_observer,
        "replacement_retire_after_latch": distribution(joined) if joined else None,
        "unresolved_replacements": unresolved,
        "inconsistencies": errors,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    try:
        report = summarize(load_rows(json.loads(args.capture.read_text())))
    except (OSError, ValueError, TypeError, AttributeError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))
    return bool(report["inconsistencies"])


if __name__ == "__main__":
    sys.exit(main())

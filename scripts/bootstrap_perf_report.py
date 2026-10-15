#!/usr/bin/env python3
"""Summarize acceptance timings and raw CPU captures without false module labels."""
import argparse
import bisect
from collections import Counter
import json
from pathlib import Path
import subprocess


def summarize_profile(record):
    path = Path(record["profile"])
    # The registry saves the exact kernel booted for this profile.
    kernel = Path(record["kernel"]) if "kernel" in record else path.parent.parent / "kernel.elf"
    output = subprocess.check_output(["nm", "-n", "-S", "-C", "--defined-only", str(kernel)], text=True)
    functions = {}
    for line in output.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and parts[2] in {"T", "t"}:
            functions[int(parts[0], 16)] = (int(parts[1], 16), parts[3])
    addresses = sorted(functions)
    kinds, symbols, pids = Counter(), Counter(), Counter()
    switches, dropped = 0, None
    for line in path.read_text().splitlines():
        parts = line.split()
        if parts and parts[0] == "CS":
            switches += 1
        elif parts and parts[0] == "PROF_END":
            dropped = int(parts[2])
        elif len(parts) == 6 and parts[0] == "P":
            address = int(parts[5].split(";")[0], 16)
            pids[parts[2]] += 1
            if int(parts[4]) & 1:
                kinds["user"] += 1
                continue
            index = bisect.bisect_right(addresses, address) - 1
            if index >= 0:
                base = addresses[index]
                size, symbol = functions[base]
                if base <= address < base + size:
                    kinds["kernel"] += 1
                    symbols[symbol] += 1
                    continue
            kinds["unresolved_module_or_kernel"] += 1
    idle = sum(symbols[name] for name in ("brook::IdleLoop()", "brook::SchedulerStartAp()"))
    busy = Counter({name: count for name,count in symbols.items() if name not in ("brook::IdleLoop()", "brook::SchedulerStartAp()")})
    return {"idle_leaf_samples": idle, "top_non_idle_kernel_leaf_samples": busy.most_common(10), "stage": record["stage"], "samples": sum(kinds.values()),
            "sample_kinds": dict(kinds), "context_switches": switches,
            "dropped_samples": dropped, "top_kernel_leaf_samples": symbols.most_common(10),
            "pid_samples": dict(pids), "profile": str(path),
            "interpretation": "Leaf CPU samples, not inclusive time; synchronous storage waits consume CPU. Unknown addresses retain no nearest-kernel-symbol attribution."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    args = parser.parse_args()
    reports = []
    for directory in args.results:
        final = directory / "result.json"
        data = json.loads((final if final.exists() else directory / "progress.json").read_text())
        manual = directory / "manual-profiles.json"
        extra_profiles = json.loads(manual.read_text()) if manual.exists() else []
        used = [s["stats"]["used_memory_bytes"] for s in data.get("snapshots", [])
                if "used_memory_bytes" in s.get("stats", {})]
        reports.append({"directory": str(directory), "complete": data.get("success", False),
                        "mode": data.get("mode"), "revision": data.get("revision"),
                        "guest_stage_seconds": {s["name"]: s.get("guest_seconds") for s in data["stages"]},
                        "observed_used_memory_bytes": {"min": min(used), "max": max(used)} if used else None,
                        "profiles": [summarize_profile(p) for p in data.get("profiles", [])+extra_profiles if "profile" in p]})
    print(json.dumps(reports, indent=2))


if __name__ == "__main__":
    main()

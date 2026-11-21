#!/usr/bin/env python3
"""Summarize a Wayland buffer capture on its guest monotonic clock."""

import argparse
from collections import Counter, defaultdict
import json
from math import ceil
from pathlib import Path
from statistics import median


SCHEMA = "brook-wayland-buffer-trace/1"


def timing(samples):
    samples = sorted(samples)
    if not samples:
        return {"count": 0}
    return {
        "count": len(samples),
        "p50_us": median(samples),
        "p95_us": samples[ceil(len(samples) * 0.95) - 1],
        "max_us": samples[-1],
    }


def backing(buffer):
    if buffer["kind"] == "dmabuf" and buffer["inode"]:
        return ("dmabuf", buffer["device"], buffer["inode"])
    return ("resource", buffer["id"])


def summarize(trace, surface=None):
    if trace["schema"] != SCHEMA:
        raise ValueError(f"unsupported trace schema {trace['schema']!r}")
    events = trace["events"]
    start, end = trace["start_us"], trace["end_us"]
    if start > end or any(not start <= row["us"] <= end for row in events):
        raise ValueError("event outside the capture interval")
    if any(a["us"] > b["us"] for a, b in zip(events, events[1:])):
        raise ValueError("event timestamps are out of order")

    buffers = {row["id"]: row for row in trace["buffers"]}
    selected = {
        (row["buffer"], row["cycle"]) for row in events
        if row["event"] == "commit" and (surface is None or row["surface"] == surface)
    }
    observed_backings = {backing(buffers[resource]) for resource, _ in selected}
    if not observed_backings and surface is not None:
        raise ValueError(f"no committed buffers for surface {surface}")
    tracked_attaches = {
        row["attach"] for row in events
        if row["event"] == "commit" and (row["buffer"], row["cycle"]) in selected
    }

    active = {}
    busy = Counter()
    peak = pool_releases = unmatched_releases = shared_cycles = all_busy = 0
    censored_starts = 0
    last_release = {}
    attachments = {}
    offer_times = {}
    surface_commit = {}
    counts = Counter()
    samples = defaultdict(list)
    for row in events:
        resource, when, kind, attach = (row[key] for key in ("buffer", "us", "event", "attach"))
        target_surface = surface is None or surface == row["surface"]
        if kind == "attach":
            if target_surface:
                counts[kind] += 1
            if attach in tracked_attaches:
                attachments[attach] = {"attach": when, "target": target_surface}
            continue

        cycle = (resource, row["cycle"])
        if cycle not in selected:
            if surface is None and kind == "release_queued":
                unmatched_releases += 1
            continue
        identity = backing(buffers[resource])
        if kind == "release_queued":
            if cycle not in active:
                unmatched_releases += 1
                continue
            begun, cycle_kind, complete = active.pop(cycle)
            if busy[identity] == 1 and len(busy) == len(observed_backings):
                pool_releases += 1
            busy[identity] -= 1
            if not busy[identity]:
                del busy[identity]
                last_release[identity] = when
            counts[kind] += 1
            if complete:
                samples[f"commit_to_release_{cycle_kind}"].append(when - begun)
            continue

        state = attachments.get(attach)
        if kind == "commit":
            shared_cycles += not target_surface
            if cycle not in active:
                other = last_release.get(identity)
                if target_surface and identity not in busy and other is not None:
                    samples["release_to_reuse_commit"].append(when - other)
                    if state and state["attach"] >= other:
                        samples["release_to_reuse_attach"].append(state["attach"] - other)
                complete = not row["uses"]
                censored_starts += not complete
                active[cycle] = (when, "unclassified", complete)
                already_full = len(busy) == len(observed_backings)
                busy[identity] += 1
                peak = max(peak, len(busy))
                all_busy += not already_full and len(busy) == len(observed_backings)
            if target_surface:
                prev = surface_commit.get(row["surface"])
                if prev is not None:
                    samples["commit_interval"].append(when - prev)
                surface_commit[row["surface"]] = when
                if state:
                    samples["attach_to_commit"].append(when - state["attach"])
        if not state or not state["target"]:
            continue
        counts[kind] += 1
        if kind in ("offer", "cpu_copy", "renderer_retired", "producer_ready"):
            if kind not in state:
                state[kind] = when
                samples[f"attach_to_{kind}"].append(when - state["attach"])
                if kind == "renderer_retired" and "offer" in state:
                    samples["first_offer_to_renderer_retired"].append(when - state["offer"])
            if kind in ("offer", "cpu_copy") and cycle in active:
                begun, prior, complete = active[cycle]
                route = "offered" if kind == "offer" or prior == "offered" else "cpu_copy"
                active[cycle] = (begun, route, complete)

        offer = (attach, row["window"], row["incarnation"], row["offer"])
        if kind == "offer":
            offer_times.setdefault(offer, when)
        if kind in ("renderer_presented", "renderer_safe") and offer in offer_times:
            samples[f"offer_to_{kind}"].append(when - offer_times[offer])

    known = {key for key in observed_backings if key[0] == "dmabuf"}
    return {
        "pid": trace["pid"],
        "surface": surface,
        "duration_us": end - start,
        "counts": dict(counts),
        "overwritten_events": trace["overwritten_events"],
        "untracked_attaches": trace["untracked_attaches"],
        "observed_buffer_resources": len({resource for resource, _ in selected}),
        "observed_known_dmabuf_backings": len(known),
        "observed_storage_keys": len(observed_backings),
        "peak_in_use_storage_keys": peak,
        "times_all_observed_storage_busy": all_busy,
        "releases_when_all_observed_storage_busy": pool_releases,
        "unmatched_releases": unmatched_releases,
        "preexisting_cycles": censored_starts,
        "unreleased_cycles_at_end": len(active),
        "other_surface_attaches_in_selected_cycles": shared_cycles,
        "timings": {name: timing(values) for name, values in sorted(samples.items())},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--surface", type=int, help="Wayland surface object ID of the target video")
    args = parser.parse_args()
    try:
        result = summarize(json.loads(args.trace.read_text()), args.surface)
    except (KeyError, TypeError, ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

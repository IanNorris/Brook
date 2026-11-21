#!/usr/bin/env python3
"""Summarize a Brook prof frame capture and optionally export a Perfetto timeline."""

import argparse
import bisect
from collections import Counter, defaultdict
from dataclasses import dataclass
import json
import math
from pathlib import Path

SWITCH, READY = 0, 1
BEGIN, END, OBSERVED, FUTEX_WAKE, GPU_WAKE, PUBLISHED = range(5, 11)
IOCTL_BASE = 0x100
IOCTLS = {
    0x59: "I915_GEM_THROTTLE", 0x5C: "I915_GEM_PREAD", 0x5D: "I915_GEM_PWRITE",
    0x5F: "I915_GEM_SET_DOMAIN", 0x69: "I915_GEM_EXECBUFFER2", 0x6C: "I915_GEM_WAIT",
    0xC3: "SYNCOBJ_WAIT", 0xCA: "SYNCOBJ_TIMELINE_WAIT",
}
STAGES = {
    READY: "ready", 2: "mutex wait", 3: "mutex grant", OBSERVED: "fence observed",
    FUTEX_WAKE: "futex wake", GPU_WAKE: "GPU wake request", PUBLISHED: "fence published",
    16: "GPU queued", 17: "GPU taken", 18: "GPU prepare", 21: "GPU device begin",
    22: "GPU device end", 25: "GPU retired", 27: "GPU job fence signal",
    28: "syncobj waiting", 34: "render issue", 35: "render breadcrumb",
    36: "render context retired",
}


@dataclass(frozen=True)
class Event:
    ticks: int
    cpu: int
    kind: int
    tid: int
    peer: int
    flags: int
    data: tuple[int, ...]


def wait_name(source):
    if source >= IOCTL_BASE:
        number = source - IOCTL_BASE
        return IOCTLS.get(number, f"i915 ioctl 0x{number:02x}")
    return {1: "sync_file poll", 2: "sync_file epoll", 3: "futex"}.get(source, f"wait {source}")


def parse(lines):
    origin = khz = tgid = None
    footer = None
    events = []
    for line in lines:
        fields = line.split()
        if not fields:
            continue
        if footer is not None:
            raise ValueError("unexpected data after PROF_END")
        if fields[0] == "FRAME_CLOCK":
            if origin is not None or len(fields) != 4:
                raise ValueError("expected one FRAME_CLOCK record")
            origin, khz, tgid = int(fields[1], 16), int(fields[2]), int(fields[3])
        elif fields[0] == "T":
            if len(fields) < 7:
                raise ValueError("malformed trace event")
            event = Event(int(fields[1], 16), int(fields[2]), int(fields[3]),
                          int(fields[4], 16), int(fields[5], 16), int(fields[6]),
                          tuple(int(value, 16) for value in fields[7:]))
            if event.kind not in (SWITCH, 4) and len(event.data) != 2:
                raise ValueError("malformed trace point")
            events.append(event)
        elif fields[0] == "PROF_END":
            if len(fields) != 3:
                raise ValueError("malformed PROF_END")
            footer = int(fields[1]), int(fields[2])
        elif fields[0] != "PROF_BEGIN":
            raise ValueError(f"unexpected profiler record {fields[0]!r}")
    if not origin or not khz or not tgid or not footer:
        raise ValueError("expected a completed targeted profiler capture")
    if footer[0] != len(events) or footer[1] < 0:
        raise ValueError("profiler event count mismatch")
    events.sort(key=lambda event: event.ticks)
    return origin, khz, tgid, footer[1], events


def distribution(values):
    if not values:
        return {"count": 0}
    values = sorted(values)

    def quantile(q):
        return round(values[math.ceil(q * len(values)) - 1], 3)

    return {"count": len(values), "p50_us": quantile(.50), "p95_us": quantile(.95),
            "max_us": round(values[-1], 3)}


def next_after(events, tick, limit=None):
    index = bisect.bisect_left(events, tick, key=lambda event: event.ticks)
    if index < len(events) and (limit is None or events[index].ticks <= limit):
        return events[index]
    return None


def analyze(parsed, include_timeline=False):
    origin, khz, tgid, dropped, events = parsed

    def to_us(delta):
        return delta * 1000 / khz

    running = defaultdict(list)
    readied = defaultdict(list)
    starts = {}
    spans = defaultdict(list)
    ioctl_results = Counter()
    kinds = Counter()
    observation = {}
    publications = {}
    wakes = []
    unmatched = 0
    pre_window = 0
    timeline = []

    for event in events:
        if event.ticks < origin:
            pre_window += 1
            continue
        kinds[event.kind] += 1
        if event.kind == SWITCH:
            running[event.peer].append(event)
        elif event.kind == READY:
            readied[event.tid].append(event)
        elif event.kind in (BEGIN, END):
            source = event.data[0]
            key = event.tid, source
            if event.kind == BEGIN:
                unmatched += key in starts
                starts[key] = event
            elif key not in starts:
                unmatched += 1
            else:
                start = starts.pop(key)
                name = wait_name(source)
                elapsed = to_us(event.ticks - start.ticks)
                spans[name].append(elapsed)
                if source >= IOCTL_BASE:
                    code = event.data[1]
                    code = code if code < (1 << 63) else code - (1 << 64)
                    ioctl_results[f"{name}: {code}"] += 1
                if include_timeline:
                    timeline.append({"name": name, "ph": "X", "pid": tgid, "tid": event.tid,
                                     "ts": to_us(start.ticks - origin), "dur": elapsed})
        elif event.kind == PUBLISHED and event.data[0]:
            publications[event.data[0]] = event
        elif event.kind == OBSERVED:
            observation.setdefault((event.tid, event.data[0]), event.data[1] / 1000)
        elif event.kind in (GPU_WAKE, FUTEX_WAKE):
            wakes.append(event)

        if include_timeline and event.kind in STAGES:
            timeline.append({"name": STAGES[event.kind], "ph": "i", "s": "t",
                             "pid": tgid, "tid": event.tid, "ts": to_us(event.ticks - origin),
                             "args": {"peer_tid": event.peer, "cpu": event.cpu,
                                      "id": event.data[0], "value": event.data[1]}})
        if include_timeline and event.kind == SWITCH:
            timeline.append({"name": "scheduled in", "ph": "i", "s": "t",
                             "pid": tgid, "tid": event.peer, "ts": to_us(event.ticks - origin),
                             "args": {"cpu": event.cpu, "from_tid": event.tid,
                                      "from_state": event.flags,
                                      "outgoing_stack": [hex(pc) for pc in event.data]}})

    signal_to_gpu_wake = []
    wake_to_ready = defaultdict(list)
    ready_to_run = defaultdict(list)
    matched_ready = set()
    for wake in wakes:
        kind = "futex" if wake.kind == FUTEX_WAKE else "gpu"
        if wake.kind == GPU_WAKE:
            published = publications.get(wake.data[0])
            if published and published.ticks <= wake.ticks:
                signal_to_gpu_wake.append(to_us(wake.ticks - published.ticks))
        run = next_after(running[wake.tid], wake.ticks)
        ready = next_after(readied[wake.tid], wake.ticks, run.ticks if run else None)
        if ready and run and ready.peer == wake.peer and ready not in matched_ready:
            matched_ready.add(ready)
            wake_to_ready[kind].append(to_us(ready.ticks - wake.ticks))
            ready_to_run[kind].append(to_us(run.ticks - ready.ticks))

    return {
        "target_tgid": tgid, "tsc_khz": khz, "events": len(events), "dropped": dropped,
        "pre_window_events": pre_window, "kinds": dict(kinds),
        "unpaired_wait_boundaries": unmatched + len(starts),
        "waits": {source: distribution(spans[source]) for source in sorted(spans)},
        "ioctl_results": dict(ioctl_results),
        "fence_resolution_to_first_observation": distribution(observation.values()),
        "fence_publication_to_gpu_wake_request": distribution(signal_to_gpu_wake),
        "wake_request_to_ready": {kind: distribution(wake_to_ready[kind]) for kind in ("gpu", "futex")},
        "ready_to_run": {kind: distribution(ready_to_run[kind]) for kind in ("gpu", "futex")},
        "notes": [
            "GPU wake requests can be broadcasts. A waiter may resume for a different fence.",
            "Futex and fence timings share a clock without implying a causal dependency.",
            "Unpaired boundaries can be clipped by the capture interval. Dropped events impair pairing.",
        ],
    }, {"traceEvents": timeline, "displayTimeUnit": "ms"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--timeline", type=Path, help="write Chrome Trace JSON for Perfetto")
    args = parser.parse_args()
    with args.profile.open() as profile:
        summary, timeline = analyze(parse(profile), args.timeline is not None)
    print(json.dumps(summary, indent=2))
    if args.timeline:
        args.timeline.write_text(json.dumps(timeline) + "\n")


if __name__ == "__main__":
    main()

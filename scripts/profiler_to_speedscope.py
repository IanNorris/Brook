#!/usr/bin/env python3
"""Convert Brook OS profiler serial output to Speedscope JSON format.

Usage:
    python3 profiler_to_speedscope.py serial.log [output] [--elf kernel.elf] [--symmap symbols.txt] [--folded]

    --elf <path>      Resolve symbols from kernel ELF (auto-detected from build/ if omitted)
    --symmap <path>   Use pre-generated symbol map (addr name, one per line)
    --folded          Output folded stacks format instead of speedscope JSON
                      (compatible with flamegraph.pl and speedscope's import)

The serial log contains lines like:
    PROF_BEGIN <cpuCount> <startTick>
    P  <tick_dec> <pid_hex> <cpu> <flags> <rip_hex>;<rip_hex>;...
    CS <tick_dec> <cpu> <old_pid_hex> <new_pid_hex>
    ...
    PROF_END <totalSamples> <dropped>

Output: Speedscope JSON (https://www.speedscope.app/file-format-schema.json)
  - One profile per CPU (sampled type)
  - One profile per PID with enough samples (sampled type)
  - A "Context Switches" timeline profile showing scheduler activity
  - Frames are unique RIP addresses (hex), optionally resolved via symmap

If a kernel symbol map is provided (--symmap FILE), RIP addresses in the
kernel range are resolved to symbol names.  The symbol map format is one
line per symbol:  <hex_addr> <name>
"""

import json
import sys
import os
import re
from collections import defaultdict


def load_symmap(path):
    """Load symbol map: {addr: name}"""
    syms = {}
    if not path:
        return syms
    with open(path) as f:
        for line in f:
            parts = line.strip().split(None, 1)
            if len(parts) == 2:
                try:
                    addr = int(parts[0], 16)
                    syms[addr] = parts[1]
                except ValueError:
                    continue
    return syms


def resolve_rip(rip, syms, sorted_addrs):
    """Resolve RIP to nearest symbol name, or hex string."""
    if not sorted_addrs:
        return f"0x{rip:016x}"
    lo, hi = 0, len(sorted_addrs) - 1
    if rip < sorted_addrs[0]:
        return f"0x{rip:016x}"
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if sorted_addrs[mid] <= rip:
            lo = mid
        else:
            hi = mid - 1
    addr = sorted_addrs[lo]
    offset = rip - addr
    name = syms[addr]
    if offset == 0:
        return name
    return f"{name}+0x{offset:x}"


# Regex for sample lines: P <tick> <pid_hex> <cpu> <flags> <rip0;rip1;...>
SAMPLE_RE = re.compile(r'^P (\d+) ([0-9a-fA-F]{1,4}) (\d+) (\d) (.+)$')
# Regex for context-switch lines: CS <tick> <cpu> <old_pid_hex> <new_pid_hex> [<rip0;rip1;...>]
# The trailing stack is the OUTGOING process's kernel callstack at yield.
CS_RE = re.compile(r'^CS (\d+) (\d+) ([0-9a-fA-F]{1,4}) ([0-9a-fA-F]{1,4})(?: (.+))?$')


def parse_serial_log(path):
    """Parse profiler lines from serial log."""
    cpuCount = 0
    startTick = 0
    samples = []
    context_switches = []  # (tick, cpu, old_pid, new_pid)
    dropped = 0
    in_profile = False

    with open(path, 'rb') as f:
        for raw_line in f:
            try:
                line = raw_line.decode('ascii', errors='ignore').strip()
            except:
                continue

            if line.startswith('PROF_BEGIN'):
                parts = line.split()
                if len(parts) >= 3:
                    cpuCount = int(parts[1])
                    startTick = int(parts[2])
                in_profile = True
                continue

            if line.startswith('PROF_END'):
                parts = line.split()
                if len(parts) >= 3:
                    dropped = int(parts[2])
                in_profile = False
                continue

            if in_profile:
                m = SAMPLE_RE.match(line)
                if m:
                    tick = int(m.group(1))
                    pid = int(m.group(2), 16)
                    cpu = int(m.group(3))
                    flags = int(m.group(4))
                    rip_str = m.group(5)
                    ring = 'user' if (flags & 1) else 'kernel'
                    stack = [int(r, 16) for r in rip_str.split(';') if r]
                    samples.append((tick, pid, cpu, ring, stack))
                    continue

                m = CS_RE.match(line)
                if m:
                    tick     = int(m.group(1))
                    cpu      = int(m.group(2))
                    old_pid  = int(m.group(3), 16)
                    new_pid  = int(m.group(4), 16)
                    rip_str  = m.group(5)
                    cs_stack = [int(r, 16) for r in rip_str.split(';') if r] if rip_str else []
                    context_switches.append((tick, cpu, old_pid, new_pid, cs_stack))

    return cpuCount, startTick, samples, context_switches, dropped


def pid_label(pid):
    return f"PID {pid}" if pid != 0xFFFF else "idle"


def _symmap_from_elf(elf_path):
    """Extract demangled symbols from ELF via nm+llvm-cxxfilt, return temp file path."""
    import subprocess, tempfile
    try:
        nm = subprocess.run(['nm', '-n', elf_path], capture_output=True, text=True)
        if nm.returncode != 0:
            print(f"  nm failed: {nm.stderr.strip()}")
            return None
        # Filter text symbols and demangle
        lines = []
        for line in nm.stdout.splitlines():
            parts = line.split(None, 2)
            if len(parts) == 3 and parts[1] in ('T', 't'):
                lines.append(f"{parts[0]} {parts[2]}\n")
        demangled = subprocess.run(['llvm-cxxfilt'], input=''.join(lines),
                                   capture_output=True, text=True)
        tmp = tempfile.NamedTemporaryFile(mode='w', suffix='.symmap', delete=False)
        tmp.write(demangled.stdout if demangled.returncode == 0 else ''.join(lines))
        tmp.close()
        count = len(lines)
        print(f"  Generated symmap: {count} symbols from {os.path.basename(elf_path)}")
        return tmp.name
    except FileNotFoundError as e:
        print(f"  Symbol extraction unavailable ({e}); addresses will be raw hex")
        return None


def write_folded(outpath, samples, syms, sorted_addrs):
    """Write folded stacks format: 'frame\\tcount' per unique frame."""
    from collections import Counter
    counts = Counter()
    for _tick, _pid, _cpu, ring, stack in samples:
        if stack:
            frame = resolve_rip(stack[0], syms, sorted_addrs)
            counts[frame] += 1
    with open(outpath, 'w') as f:
        for frame, count in counts.most_common():
            f.write(f"{frame} {count}\n")
    print(f"Wrote {outpath} ({len(counts)} unique frames, {sum(counts.values())} samples)")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} serial.log [output] [--symmap symbols.txt] [--elf kernel.elf] [--folded]")
        sys.exit(1)

    inpath = sys.argv[1]
    outpath = None
    symmap_path = None
    elf_path = None
    folded = False

    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == '--symmap' and i + 1 < len(sys.argv):
            symmap_path = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == '--elf' and i + 1 < len(sys.argv):
            elf_path = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == '--folded':
            folded = True
            i += 1
        elif outpath is None and not sys.argv[i].startswith('--'):
            outpath = sys.argv[i]
            i += 1
        else:
            i += 1

    # Auto-detect ELF if not given
    if elf_path is None and symmap_path is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(script_dir)
        for candidate in [
            os.path.join(root, 'build', 'release', 'kernel', 'BROOK.elf'),
            os.path.join(root, 'build', 'debug', 'kernel', 'BROOK.elf'),
        ]:
            if os.path.exists(candidate):
                elf_path = candidate
                print(f"Auto-detected ELF: {elf_path}")
                break

    # Generate symmap from ELF using nm + llvm-cxxfilt
    if elf_path and not symmap_path:
        symmap_path = _symmap_from_elf(elf_path)

    if outpath is None:
        stem = inpath.rsplit('.', 1)[0]
        outpath = stem + '.folded' if folded else stem + '.speedscope.json'

    syms = load_symmap(symmap_path)
    sorted_addrs = sorted(syms.keys()) if syms else []

    cpuCount, startTick, samples, context_switches, dropped = parse_serial_log(inpath)

    if not samples and not context_switches:
        print("No profiler events found in log. Look for PROF_BEGIN/P/CS/PROF_END lines.")
        sys.exit(1)

    print(f"Parsed: {cpuCount} CPUs, {len(samples)} samples, "
          f"{len(context_switches)} context switches, {dropped} dropped")

    if folded:
        write_folded(outpath, samples, syms, sorted_addrs)
        return

    # Build frame table (unique RIP → index)
    frame_map = {}
    frames = []

    def get_frame(rip, ring):
        key = (rip, ring)
        if key not in frame_map:
            name = resolve_rip(rip, syms, sorted_addrs)
            if ring == 'user':
                name = f"[user] {name}"
            frame_map[key] = len(frames)
            frames.append({"name": name})
        return frame_map[key]

    # Pre-create frames for CS pid labels (so they show nicely in the timeline)
    cs_pid_frame = {}
    def get_cs_frame(pid):
        if pid not in cs_pid_frame:
            cs_pid_frame[pid] = len(frames)
            frames.append({"name": pid_label(pid)})
        return cs_pid_frame[pid]

    # Group samples by CPU and PID
    by_cpu = defaultdict(list)
    by_pid = defaultdict(list)
    for tick, pid, cpu, ring, stack in samples:
        by_cpu[cpu].append((tick, pid, ring, stack))
        by_pid[pid].append((tick, cpu, ring, stack))

    profiles = []

    def make_sample_stack(ring, stack):
        """Convert a stack trace to Speedscope frame indices (bottom-to-top)."""
        frame_indices = []
        for rip in reversed(stack):
            fi = get_frame(rip, ring)
            frame_indices.append(fi)
        return frame_indices

    # Per-CPU sampled profiles
    for cpu in sorted(by_cpu.keys()):
        cpu_samples = by_cpu[cpu]
        if not cpu_samples:
            continue
        ss = []
        weights = []
        for tick, pid, ring, stack in cpu_samples:
            ss.append(make_sample_stack(ring, stack))
            weights.append(10)  # 10ms per sample at 100Hz
        profiles.append({
            "type": "sampled",
            "name": f"CPU {cpu}",
            "unit": "milliseconds",
            "startValue": cpu_samples[0][0],
            "endValue": cpu_samples[-1][0] + 10,
            "samples": ss,
            "weights": weights,
        })

    # Per-PID sampled profiles (only PIDs with enough samples)
    for pid in sorted(by_pid.keys()):
        pid_samples = by_pid[pid]
        if len(pid_samples) < 5:
            continue
        ss = []
        weights = []
        for tick, cpu, ring, stack in pid_samples:
            ss.append(make_sample_stack(ring, stack))
            weights.append(10)
        profiles.append({
            "type": "sampled",
            "name": pid_label(pid),
            "unit": "milliseconds",
            "startValue": pid_samples[0][0],
            "endValue": pid_samples[-1][0] + 10,
            "samples": ss,
            "weights": weights,
        })

    # Context-switch timeline: one sampled profile per CPU showing which PID
    # was running at each point.  Each CS event ends the previous PID's slice
    # and starts the new one.  Weight = duration of the slice in ticks (≈ ms).
    by_cs_cpu = defaultdict(list)
    for tick, cpu, old_pid, new_pid, cs_stack in sorted(context_switches):
        by_cs_cpu[cpu].append((tick, old_pid, new_pid, cs_stack))

    for cpu in sorted(by_cs_cpu.keys()):
        events = by_cs_cpu[cpu]
        if not events:
            continue
        ss = []
        weights = []
        prev_tick = events[0][0]
        for i, (tick, old_pid, new_pid, cs_stack) in enumerate(events):
            # Slice weight = ticks since last switch
            w = max(tick - prev_tick, 1)
            ss.append([[get_cs_frame(old_pid)]])
            weights.append(w)
            prev_tick = tick
        # Final slice to end of recording
        if samples:
            last_tick = max(s[0] for s in samples)
            w = max(last_tick - prev_tick, 1)
            ss.append([[get_cs_frame(events[-1][2])]])
            weights.append(w)

        profiles.append({
            "type": "sampled",
            "name": f"Scheduler CPU {cpu} (context switches)",
            "unit": "milliseconds",
            "startValue": events[0][0],
            "endValue": (events[-1][0] + weights[-1]) if ss else events[-1][0],
            "samples": ss,
            "weights": weights,
        })

    # Summary: per-PID time spent running (from CS events)
    pid_run_ms = defaultdict(int)
    for cpu_events in by_cs_cpu.values():
        for i, ev in enumerate(cpu_events):
            tick = ev[0]
            if i > 0:
                prev = cpu_events[i-1]
                pid_run_ms[prev[2]] += tick - prev[0]
    if pid_run_ms:
        print("\nPID run time from context switches (ticks ≈ ms):")
        for pid, ms in sorted(pid_run_ms.items(), key=lambda x: -x[1]):
            print(f"  {pid_label(pid):12s} {ms:6d} ms")

    # Per-PID yield-stack histogram: when a PID context-switches OUT, what
    # kernel call site did it yield from? Top entries point at the blocking
    # primitive each PID is parked on (KMutex, futex, VirtioBlk poll, etc.).
    yield_stacks = defaultdict(lambda: defaultdict(int))
    for cpu_events in by_cs_cpu.values():
        for tick, old_pid, new_pid, cs_stack in cpu_events:
            if cs_stack:
                # Use leaf RIP as the bucketing key.
                yield_stacks[old_pid][cs_stack[0]] += 1
    if yield_stacks:
        print("\nTop yield sites per PID (where each pid kept context-switching out):")
        for pid in sorted(yield_stacks.keys(),
                          key=lambda p: -sum(yield_stacks[p].values()))[:20]:
            sites = sorted(yield_stacks[pid].items(), key=lambda x: -x[1])
            total = sum(c for _, c in sites)
            top3 = sites[:3]
            print(f"  {pid_label(pid):12s} ({total} CS):")
            for rip, count in top3:
                print(f"      0x{rip:016x}  x{count}")

    speedscope = {
        "$schema": "https://www.speedscope.app/file-format-schema.json",
        "shared": {"frames": frames},
        "profiles": profiles,
        "name": f"Brook OS Profile ({len(samples)} samples, {len(context_switches)} CS, {cpuCount} CPUs)",
        "activeProfileIndex": 0,
        "exporter": "brook-profiler",
    }

    with open(outpath, 'w') as f:
        json.dump(speedscope, f, separators=(',', ':'))

    print(f"\nWrote {outpath} ({len(frames)} frames, {len(profiles)} profiles)")
    size_kb = os.path.getsize(outpath) / 1024
    print(f"  {size_kb:.1f} KB — open at https://www.speedscope.app/")


if __name__ == '__main__':
    main()


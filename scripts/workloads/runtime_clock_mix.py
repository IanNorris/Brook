#!/usr/bin/env python3
"""Finite CPU-clock/ownership diagnostic; no devices, network or licensed data.

Run as a normal guest user. Each wave forks a bounded number of processes,
each with two fresh threads alternating CPU-clock reads, arithmetic and sleeps.
The host controller owns the independent timeout and original job lifecycle.
Clock measurements include observer overhead; this is not a throughput score.
"""
import argparse
import json
import os
from pathlib import Path
import threading
import time


def thread_probe(seconds, index):
    wall_start = time.monotonic_ns()
    cpu_start = previous_cpu = time.thread_time_ns()
    previous_process = time.process_time_ns()
    previous_wall = wall_start
    end = wall_start + int(seconds * 1e9)
    reads = sleeps = backwards = early = 0
    value = index + 1
    while time.monotonic_ns() < end:
        for _ in range(128):
            value = (value * 1664525 + 1013904223) & 0xffffffff
        cpu = time.thread_time_ns()
        process = time.process_time_ns()
        wall = time.monotonic_ns()
        backwards += int(cpu < previous_cpu or process < previous_process or wall < previous_wall)
        previous_cpu, previous_process, previous_wall = cpu, process, wall
        reads += 1
        if reads % 17 == 0:
            before = time.monotonic_ns()
            time.sleep(.001)
            early += int(time.monotonic_ns() - before < 1_000_000)
            sleeps += 1
    cpu_end = time.thread_time_ns()
    wall_end = time.monotonic_ns()
    return dict(thread=index, reads=reads, sleeps=sleeps, backwards=backwards,
                early_sleeps=early, signature=value, wall_start_ns=wall_start,
                wall_end_ns=wall_end, cpu_ns=cpu_end-cpu_start,
                cpu_exceeds_wall=cpu_end-cpu_start > wall_end-wall_start)


def child(seconds):
    results = [None, None]
    failures = []

    def run(index):
        try:
            results[index] = thread_probe(seconds, index)
        except BaseException as error:
            failures.append(repr(error))

    threads = [threading.Thread(target=run, args=(i,)) for i in range(2)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return dict(pid=os.getpid(), threads=results, failures=failures)


def wave(workers, seconds):
    children = []
    start = time.monotonic_ns()
    # Parent has no live Python threads at fork; each child creates its own.
    for _ in range(workers):
        reader, writer = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(reader)
            try:
                body = json.dumps(child(seconds)).encode()
                offset = 0
                while offset < len(body):
                    offset += os.write(writer, body[offset:])
                os.close(writer)
                os._exit(0)
            except BaseException:
                os._exit(1)
        os.close(writer)
        children.append((pid, reader))
    results = []
    for pid, reader in children:
        chunks = []
        while True:
            chunk = os.read(reader, 4096)
            if not chunk:
                break
            chunks.append(chunk)
        os.close(reader)
        collected, status = os.waitpid(pid, 0)
        assert collected == pid and status == 0, (pid, status)
        result = json.loads(b''.join(chunks))
        assert result['pid'] == pid
        results.append(result)
    return dict(workers=workers, seconds=seconds, start_ns=start,
                end_ns=time.monotonic_ns(), children=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workers', type=int, nargs='+', default=[1, 8])
    parser.add_argument('--seconds', type=float, default=6)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.seconds <= 10 or not 1 <= len(args.workers) <= 4 or any(
            not 1 <= n <= 16 for n in args.workers):
        parser.error('Use 1–4 waves, 1–16 processes per wave, and 0–10 seconds per wave')
    report = dict(waves=[], scope='Finite ownership/CPU-clock diagnostic, not observer-free performance')
    print('RUNTIME_CLOCK_MIX_STARTED', flush=True)
    for workers in args.workers:
        result = wave(workers, args.seconds)
        report['waves'].append(result)
        print('WAVE_COMPLETE', workers, flush=True)
        # Allow delayed service corrections after workers have retired.
        time.sleep(.5)
    rows = [row for w in report['waves'] for c in w['children'] for row in c['threads'] if row]
    report['passed'] = (len(rows) == 2*sum(args.workers)
                        and not any(c['failures'] for w in report['waves'] for c in w['children'])
                        and all(r['reads'] and r['sleeps'] and not r['backwards']
                                and not r['early_sleeps'] and not r['cpu_exceeds_wall'] for r in rows))
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print('RUNTIME_CLOCK_MIX_RESULT', json.dumps(dict(passed=report['passed'], threads=len(rows))), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

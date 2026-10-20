#!/usr/bin/env python3
"""Bounded CPU-clock API probe: sleeping, running, exited siblings and fork.

Includes libc/vDSO and raw x86-64 syscalls. This checks standard clock IDs 2/3,
not POSIX CPU timers, dynamic encoded clocks or user/system/IRQ separation.
"""
import ctypes as c
import errno
import json
import os
import platform
import threading
import time


class Stamp(c.Structure):
    _fields_ = [('sec', c.c_long), ('nsec', c.c_long)]


def main():
    if platform.machine().lower() not in ('x86_64', 'amd64'):
        raise SystemExit('Linux x86-64 syscall numbers required')
    libc = c.CDLL(None, use_errno=True)
    libc.syscall.restype = c.c_long
    libc.clock_gettime.argtypes = [c.c_int, c.POINTER(Stamp)]
    libc.clock_gettime.restype = c.c_int
    checks, rows = [], {}

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))

    def clock(clockid, raw=False):
        value = Stamp()
        if raw:
            result = libc.syscall(c.c_long(228), c.c_long(clockid), c.byref(value))
        else:
            result = libc.clock_gettime(clockid, c.byref(value))
        if result:
            raise OSError(c.get_errno(), 'clock_gettime', clockid)
        return value.sec * 1_000_000_000 + value.nsec

    def sample():
        return dict(wall=clock(1), process=clock(2), thread=clock(3))

    def delta(before, after):
        return {k: after[k]-before[k] for k in before}

    def burn(seconds):
        end = clock(1) + int(seconds * 1e9)
        signature = 0
        while clock(1) < end:
            signature ^= sum(range(1200))
        return signature

    # Optional host gate lets an already-warmed interpreter start measurements
    # only after bounded host workers are ready. This controls the private VM;
    # the URL is supplied by its experiment owner, never discovered externally.
    gate_url = os.environ.get('BROOK_CPU_TIME_GATE_URL')
    if gate_url:
        import urllib.request
        print('CPU_TIME_READY', flush=True)
        gate_deadline = time.monotonic()+90
        while True:
            with urllib.request.urlopen(gate_url, timeout=3) as response:
                gate = json.load(response)
            if gate['ready']:
                rows['host_gate'] = gate
                break
            if time.monotonic() >= gate_deadline:
                raise TimeoutError('Host gate did not open; do not relaunch this job')
            time.sleep(.05)
    first = sample()
    time.sleep(.25)
    slept = delta(first, sample())
    rows['sleep'] = slept
    check('sleep_wall_advanced', slept['wall'] >= 200_000_000)
    check('sleep_thread_not_wall', 0 <= slept['thread'] < slept['wall']//4)
    check('sleep_process_not_wall', 0 <= slept['process'] < slept['wall']//4)
    before = sample()
    burn(.2)
    busy = delta(before, sample())
    rows['busy'] = busy
    check('busy_thread_advanced', 10_000_000 < busy['thread'] <= busy['wall']*11//10+5_000_000)
    check('busy_process_includes_thread', busy['process']+2_000_000 >= busy['thread'])

    for clockid in (2, 3):
        a, b, d = clock(clockid), clock(clockid, True), clock(clockid)
        check('libc_raw_bracket_'+str(clockid), a <= b <= d)
        stamp = Stamp()
        result = libc.syscall(c.c_long(229), c.c_long(clockid), c.byref(stamp))
        check('resolution_'+str(clockid), result == 0 and 0 < stamp.sec*1_000_000_000+stamp.nsec <= 1_000_000)

    # All workers finish before reading the group total: terminated siblings
    # must not disappear from their still-live process's accumulated CPU time.
    barrier = threading.Barrier(4)
    workers = []
    errors = []
    def worker(index):
        try:
            barrier.wait(timeout=5)
            a = clock(3)
            signature = burn(.2)
            workers.append(dict(index=index, thread_ns=clock(3)-a, signature=signature))
        except BaseException as error:
            errors.append(repr(error))
    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(3)]
    before = sample()
    for thread in threads:
        thread.start()
    barrier.wait(timeout=5)
    for thread in threads:
        thread.join(5)
    after = sample()
    group = delta(before, after)
    worker_total = sum(row['thread_ns'] for row in workers)
    rows['siblings'] = dict(delta=group, workers=sorted(workers, key=lambda row: row['index']),
                            worker_total_ns=worker_total, errors=errors)
    check('workers_drained', not errors and len(workers) == 3 and not any(t.is_alive() for t in threads))
    check('group_includes_exited_siblings', group['process']+5_000_000 >= worker_total)
    check('siblings_used_cpu', worker_total > 20_000_000)
    check('group_exceeds_main_thread', group['process'] > group['thread']+worker_total//2)
    before = sample()
    time.sleep(.15)
    retained = delta(before, sample())
    rows['after_join_sleep'] = retained
    check('group_retained_and_quiet', 0 <= retained['process'] < retained['wall']//4)

    read_fd, write_fd = os.pipe()
    parent_cpu = clock(2)
    child = os.fork()
    if child == 0:
        os.close(read_fd)
        result = dict(process_ns=clock(2), thread_ns=clock(3))
        os.write(write_fd, json.dumps(result).encode())
        os.close(write_fd)
        os._exit(0)
    os.close(write_fd)
    child_bytes = os.read(read_fd, 2048)
    os.close(read_fd)
    found, status = os.waitpid(child, 0)
    child_clock = json.loads(child_bytes)
    rows['fork'] = dict(parent_process_ns=parent_cpu, child=child_clock, status=status)
    check('fork_clean_exit', found == child and status == 0)
    check('fork_does_not_inherit_cpu', child_clock['process_ns'] < parent_cpu//2+20_000_000)

    # An invalid clock must be rejected, rather than silently return uptime.
    stamp = Stamp()
    c.set_errno(0)
    result = libc.syscall(c.c_long(228), c.c_long(-1), c.byref(stamp))
    check('invalid_clock_rejected', result == -1 and c.get_errno() == errno.EINVAL)
    passed = all(row['passed'] for row in checks)
    print(json.dumps(dict(passed=passed, checks=checks, measurements=rows)), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())

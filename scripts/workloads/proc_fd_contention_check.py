#!/usr/bin/env python3
"""Exercise proc FD snapshots while sibling threads churn shared descriptors.

Run in a disposable Brook VM or on Linux. Outputs only counts; never file paths
or descriptor contents. The hosted regression covers the deterministic lock
dependency; this is an SMP integration check, not a proof of deadlock freedom.
"""
import errno
import json
import os
import signal
import threading


def main():
    signal.alarm(90)
    seed = os.open('/dev/null', os.O_RDONLY)
    directory = '/proc/%d/fd' % os.getpid()
    gate = threading.Barrier(6)
    errors = []
    counts = [0] * 6

    def run(index):
        try:
            gate.wait()
            for _ in range(1000):
                if index < 4:
                    duplicate = os.dup(seed)
                    os.close(duplicate)
                else:
                    assert os.readlink('%s/%d' % (directory, seed))
                    for name in os.listdir(directory):
                        try:
                            os.readlink(directory + '/' + name)
                        except OSError as error:
                            # A churn descriptor may close between listing and lookup.
                            if error.errno != errno.ENOENT:
                                raise
                counts[index] += 1
        except BaseException as error:
            errors.append(type(error).__name__)

    workers = [threading.Thread(target=run, args=(index,)) for index in range(6)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    os.close(seed)
    signal.alarm(0)
    passed = not errors and counts == [1000] * 6
    print('PROC_FD_CONTENTION_RESULT', json.dumps(dict(passed=passed, iterations=counts,
                                                     error_types=errors)), flush=True)
    assert passed


if __name__ == '__main__':
    main()

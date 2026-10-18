#!/usr/bin/env python3
"""Bounded epoll correctness/iteration benchmark for Linux and Brook.

Run in an otherwise idle guest; record external heap counters around the job.
Does not accept general browser readiness, ET/ONESHOT or timing behavior.
"""
import argparse
import json
import os
import select
import threading
import time


def ready_calls(iterations, batch):
    event = os.eventfd(1, os.EFD_NONBLOCK)
    try:
        with select.epoll() as poll:
            poll.register(event, select.EPOLLIN)
            start = time.monotonic_ns()
            for _ in range(iterations):
                result = poll.poll(0.01, batch)
                if result != [(event, select.EPOLLIN)]:
                    raise AssertionError(result)
            elapsed = time.monotonic_ns() - start
            assert os.eventfd_read(event) == 1
            assert poll.poll(0, batch) == []
            return {'calls': iterations, 'batch': batch, 'elapsed_ns': elapsed,
                    'ns_per_call': elapsed / iterations}
    finally:
        os.close(event)


def sleepers(iterations):
    # More simultaneous waits than the 32 retained-free-object cache slots.
    # Sleepers must not prevent a completed fallback from becoming reusable.
    count = 48
    events = [os.eventfd(0, os.EFD_NONBLOCK) for _ in range(count)]
    entered = [threading.Event() for _ in events]
    errors = []
    threads = []

    def wait(index):
        try:
            with select.epoll() as poll:
                poll.register(events[index], select.EPOLLIN)
                entered[index].set()
                result = poll.poll(8, 1)
                if result != [(events[index], select.EPOLLIN)]:
                    raise AssertionError(result)
        except BaseException as error:
            errors.append(repr(error))

    try:
        for i in range(count):
            thread = threading.Thread(target=wait, args=(i,))
            thread.start(); threads.append(thread)
        for event in entered:
            if not event.wait(5): raise AssertionError('waiter setup timeout')
        time.sleep(0.05)
        report = ready_calls(iterations, 1)
    finally:
        for event in events: os.eventfd_write(event, 1)
        for thread in threads: thread.join(10)
        alive = sum(thread.is_alive() for thread in threads)
        # Do not close descriptors still owned by a failed live waiter.
        if not alive:
            for event in events: os.close(event)
    assert not alive, alive
    assert not errors, errors
    return {'waiters': count, 'hot_calls': report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iterations', type=int, default=10000)
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100000: parser.error('iterations must be 1..100000')
    report = {'ready': [ready_calls(args.iterations, batch) for batch in (1, 16, 1024)],
              'sleepers': sleepers(args.iterations)}
    print('EPOLL_CHURN_PASS', json.dumps(report), flush=True)


if __name__ == '__main__':
    main()

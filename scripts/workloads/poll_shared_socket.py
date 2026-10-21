#!/usr/bin/env python3
"""Bounded multi-poller Unix-socket notification diagnostic.

Keep each byte unread until every poller returns, so readability persists.
Timings are observations, not assertions about scheduler latency.
"""
import argparse
import json
import select
import socket
import threading
import time


def trial(mode, pollers, reverse, delay_ms):
    pairs = [socket.socketpair() for _ in range(pollers if mode == 'separate' else 1)]
    ready = [threading.Event() for _ in range(pollers)]
    results, errors = [None]*pollers, []
    threads = []

    def worker(index):
        try:
            receiver = pairs[index if mode == 'separate' else 0][0]
            poll = select.poll()
            poll.register(receiver.fileno(), select.POLLIN)
            begin = time.monotonic_ns()
            tid = threading.get_native_id()
            ready[index].set()
            events = poll.poll(1000)
            end = time.monotonic_ns()
            assert events == [(receiver.fileno(), select.POLLIN)], events
            results[index] = dict(index=index, tid=tid, begin_ns=begin, end_ns=end)
        except BaseException as error:
            errors.append(repr(error))
            ready[index].set()

    try:
        order = list(range(pollers))
        if reverse:
            order.reverse()
        for index in order:
            thread = threading.Thread(target=worker, args=(index,), daemon=True)
            threads.append(thread)
            thread.start()
            assert ready[index].wait(2), 'Worker failed to reach poll entry'
            # This is an observation aid, not proof of kernel registration;
            # raw entry times and notification bounds are retained.
            time.sleep(.003)
        time.sleep(delay_ms/1000)
        writes = []
        for _, sender in pairs:
            before = time.monotonic_ns()
            sender.sendall(b'x')
            writes.append(dict(before_ns=before, after_ns=time.monotonic_ns()))
        for thread in threads:
            thread.join(2)
        assert not any(t.is_alive() for t in threads), 'Poller did not return'
        assert not errors, errors
        for index, result in enumerate(results):
            write = writes[index if mode == 'separate' else 0]
            result['poll_elapsed_ns'] = result['end_ns']-result['begin_ns']
            result['return_after_write_bounds_ns'] = [result['end_ns']-write['after_ns'],
                                                       result['end_ns']-write['before_ns']]
        # Nobody consumed data on behalf of another waiter.
        assert all(receiver.recv(1) == b'x' for receiver, _ in pairs)
        return dict(mode=mode, reverse=reverse, launch_order=order, writes=writes, pollers=results)
    finally:
        for receiver, sender in pairs:
            receiver.close()
            sender.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--pollers', type=int, default=2)
    parser.add_argument('--write-delay-ms', type=int, default=20)
    args = parser.parse_args()
    if not (1 <= args.rounds <= 20 and 2 <= args.pollers <= 8 and 1 <= args.write_delay_ms <= 100):
        parser.error('Use rounds1..20, pollers2..8 and write delay1..100ms')
    results = []
    for mode, count in [('single', 1), ('shared', args.pollers), ('separate', args.pollers)]:
        for iteration in range(args.rounds):
            results.append(trial(mode, count, bool(iteration % 2), args.write_delay_ms))
    print(json.dumps(dict(trials=results, scope='Persistent readability must be observed by all '
                         'pollers. Entry markers do not prove kernel registration; timings include '
                         'scheduling. Compare shared endpoint against single/separate controls, '
                         'retain both registration orders and raw writer timestamp bounds.')))


if __name__ == '__main__':
    main()

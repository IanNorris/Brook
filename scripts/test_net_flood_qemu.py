#!/usr/bin/env python3
"""Keep the debug bridge reachable while a guest app floods unread UDP sockets.

The caller supplies an already booted disposable Brook VM with Python
(`/nix/profile/bin/python3`) and its forwarded debug-listener port. A host
UDP echo server reflects every guest datagram several times into guest
sockets that the guest app never reads, so their receive buffers fill and the
kernel drops on every arrival. Meanwhile every bridge query opens a fresh TCP
connection (as the bridge does), and `bridge net arp flush` periodically
empties the neighbor table, so the SYN-ACK and ACKs sent from receive
processing must resolve 10.0.2.2 without waiting (the 2026-11-16 Dell
failure mode). The run passes when no bridge query fails, the slowest stays
under --max-latency, and the guest counters show both paths were exercised.

    python3 scripts/test_net_flood_qemu.py --port 21237 --output build/net-flood/result.json
"""
import argparse
import json
import secrets
import socket
import struct
import threading
import time
from pathlib import Path

from test_portable_guest import Guest

FLOOD = r'''
import os, socket, sys, time
duration, port, count = float(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
sockets = []
for _ in range(count):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.setblocking(False)
    sockets.append(s)
payload = os.urandom(1200)
print("READY", flush=True)
end = time.monotonic() + duration
sent = errors = 0
while time.monotonic() < end:
    for s in sockets:          # never read: replies accumulate until dropped
        try:
            s.sendto(payload, ("10.0.2.2", port)); sent += 1
        except OSError:
            errors += 1
    pass
print("SENT", sent, "ERRORS", errors, flush=True)
'''


def echo_server(sock, stop, amplify, stats):
    sock.settimeout(0.2)
    while not stop.is_set():
        try:
            data, peer = sock.recvfrom(4096)
        except socket.timeout:
            continue
        stats['received'] += 1
        for _ in range(amplify):
            try:
                sock.sendto(data, peer)
                stats['echoed'] += 1
            except OSError:
                stats['echo_errors'] += 1


def start_job(guest, argv):
    ident = secrets.token_hex(16)
    payload = b'BJ01' + struct.pack('<III', 1000, len(argv), 0)
    payload += b''.join(x.encode() + b'\0' for x in argv + ['/tmp'])
    guest.query(f'bridge job begin {ident} {len(payload)} {guest.boot}')
    for offset in range(0, len(payload), 64):
        guest.query(f'bridge job put {ident} {offset} {payload[offset:offset+64].hex()} {guest.boot}')
    guest.query(f'bridge job commit {ident} {guest.boot}')
    return ident


def job_output(guest, ident):
    status = guest.query(f'bridge job status {ident} {guest.boot}')
    output = bytes.fromhex(guest.query(f'bridge job read {ident} out 0 1024 {guest.boot}')['hex'])
    return status, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True, help='forwarded guest debug-listener port')
    parser.add_argument('--echo-port', type=int, default=0, help='host UDP echo port (default: ephemeral)')
    parser.add_argument('--duration', type=float, default=60.0)
    parser.add_argument('--sockets', type=int, default=32)
    parser.add_argument('--amplify', type=int, default=3)
    parser.add_argument('--flush-every', type=float, default=2.0)
    parser.add_argument('--max-latency', type=float, default=5.0)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    echo.bind(('127.0.0.1', args.echo_port))
    echo_port = echo.getsockname()[1]
    stop = threading.Event()
    echo_stats = {'received': 0, 'echoed': 0, 'echo_errors': 0}
    thread = threading.Thread(target=echo_server, args=(echo, stop, args.amplify, echo_stats), daemon=True)
    thread.start()

    guest = Guest(args.port)
    before = guest.query('bridge net stats')
    ident = start_job(guest, ['/nix/profile/bin/python3', '-c', FLOOD,
                              str(args.duration), str(echo_port), str(args.sockets)])
    queries = failures = flushes = 0
    latencies, errors = [], []
    started = time.monotonic()
    next_flush = started + args.flush_every
    job_done = False
    while time.monotonic() - started < args.duration + 30:
        command = 'bridge net stats'
        if time.monotonic() >= next_flush:
            command, next_flush = 'bridge net arp flush', time.monotonic() + args.flush_every
        t0 = time.monotonic()
        try:
            guest.query(command)
            latencies.append(time.monotonic() - t0)
            flushes += command.endswith('flush')
        except Exception as error:  # a failed query is the failure being tested
            failures += 1
            errors.append(f'{time.monotonic() - started:.1f}s {command}: {error!r}')
        queries += 1
        if queries % 20 == 0:
            try:
                status, _ = job_output(guest, ident)
                job_done = status.get('state') in ('completed', 'failed')
            except Exception as error:
                failures += 1
                errors.append(f'job status: {error!r}')
            if job_done:
                break
        time.sleep(0.1)
    stop.set()
    thread.join()
    status, output = job_output(guest, ident)
    after = guest.query('bridge net stats')
    arp = guest.query('bridge net arp')
    delta = {k: after[k] - before.get(k, 0) for k in after if isinstance(after[k], int)}
    result = {
        'queries': queries, 'failures': failures, 'flushes': flushes, 'errors': errors[:20],
        'latency_max_s': max(latencies) if latencies else None,
        'latency_mean_s': sum(latencies) / len(latencies) if latencies else None,
        'echo': echo_stats, 'job': status, 'job_output': output.decode(errors='replace'),
        'counter_delta': delta, 'arp_after': arp,
    }
    passed = (failures == 0 and latencies and max(latencies) < args.max_latency
              and delta.get('udp_rx_full', 0) > 0 and delta.get('resolve_queued', 0) > 0
              and b'SENT' in output)
    result['pass'] = bool(passed)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('pass', 'queries', 'failures', 'flushes',
                                             'latency_max_s', 'latency_mean_s')}))
    print('counters:', json.dumps({k: delta.get(k) for k in (
        'udp_rx_full', 'resolve_queued', 'resolve_nonblocking', 'resolve_blocking_failed',
        'send_no_neighbor', 'arp_requests_sent', 'syn_queue_full', 'syn_recv_evicted')}))
    print('job:', output.decode(errors='replace').strip())
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Exercise the opt-in debug-job terminate command on a disposable Brook VM.

The caller supplies an already booted test guest with Python installed. This
uses only a new UID-1000 test job and its boot-pinned debug operation ID.
"""
import argparse
import secrets
import struct
import time

from test_portable_guest import Guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--fill-table', action='store_true',
                        help='Consume all remaining job IDs on a fresh disposable guest')
    args = parser.parse_args()
    guest = Guest(args.port)
    ident = secrets.token_hex(16)
    code = '''import signal,time
count=0
def term(sig, frame):
 global count
 count+=1
signal.signal(signal.SIGTERM,term)
print("READY",flush=True)
deadline=time.monotonic()+300
while not count and time.monotonic()<deadline: time.sleep(.01)
time.sleep(1)
print("TERMS=%d" % count,flush=True)
'''
    argv = ['/nix/profile/bin/python3', '-c', code]
    payload = b'BJ01' + struct.pack('<III', 1000, len(argv), 0)
    payload += b''.join(x.encode() + b'\0' for x in argv + ['/tmp'])
    guest.query(f'bridge job begin {ident} {len(payload)} {guest.boot}')
    for offset in range(0, len(payload), 64):
        guest.query(f'bridge job put {ident} {offset} {payload[offset:offset+64].hex()} {guest.boot}')
    guest.query(f'bridge job commit {ident} {guest.boot}')
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        status = guest.query(f'bridge job status {ident} {guest.boot}')
        output = guest.query(f'bridge job read {ident} out 0 1024 {guest.boot}')
        if b'READY\n' in bytes.fromhex(output['hex']):
            break
        if status['state'] in ('completed', 'failed'):
            raise RuntimeError('Fixture exited before READY: ' + repr(status))
        time.sleep(.05)
    else:
        raise TimeoutError('Fixture did not become ready: ' + ident)
    if args.fill_table:
        caps = guest.query('bridge job caps')
        assert caps.get('terminate_api') == 1 and caps.get('max_jobs') == 256, caps
        # Uploading jobs also consume the eight-active-job quota, so fill the
        # retained-ID table with completed, drained short jobs instead.
        no_op = ['/nix/profile/bin/python3', '-c', 'pass']
        short = b'BJ01' + struct.pack('<III', 1000, len(no_op), 0)
        short += b''.join(x.encode() + b'\0' for x in no_op + ['/tmp'])
        for index in range(caps['max_jobs'] - 1):
            new_id = secrets.token_hex(16)
            guest.query(f'bridge job begin {new_id} {len(short)} {guest.boot}')
            for offset in range(0, len(short), 64):
                guest.query(f'bridge job put {new_id} {offset} {short[offset:offset+64].hex()} {guest.boot}')
            guest.query(f'bridge job commit {new_id} {guest.boot}')
            until = time.monotonic() + 20
            while time.monotonic() < until:
                admitted = guest.query(f'bridge job status {new_id} {guest.boot}')
                if admitted['state'] in ('completed', 'failed'):
                    break
                time.sleep(.02)
            assert admitted['state'] == 'completed' and admitted['wait_status'] == 0, admitted
            assert admitted['live_tasks'] == 0, admitted
            if (index + 1) % 50 == 0:
                print('RETAINED_JOBS', index + 1, flush=True)
        try:
            guest.query(f'bridge job begin {secrets.token_hex(16)} 16 {guest.boot}')
        except RuntimeError as error:
            assert 'Job capacity exceeded' in str(error), error
        else:
            raise AssertionError('Fresh job unexpectedly admitted to full table')
    deadline = time.monotonic() + 20
    for _ in range(2):
        status = guest.query(f'bridge job terminate {ident} {guest.boot}')
        assert status['terminate_requested'] is True, status
    while time.monotonic() < deadline:
        status = guest.query(f'bridge job status {ident} {guest.boot}')
        if status['state'] in ('completed', 'failed'):
            break
        time.sleep(.05)
    assert status['state'] == 'completed' and status['wait_status'] == 0, status
    assert status['live_tasks'] == 0 and status['stdout_eof'] and status['stderr_eof'], status
    output = guest.query(f'bridge job read {ident} out 0 1024 {guest.boot}')
    assert bytes.fromhex(output['hex']) == b'READY\nTERMS=1\n', output
    repeated = guest.query(f'bridge job terminate {ident} {guest.boot}')
    assert repeated['state'] == 'completed' and repeated['terminate_requested'], repeated
    other_boot = ('1' if guest.boot[0] != '1' else '2') + guest.boot[1:]
    try:
        guest.query(f'bridge job terminate {ident} {other_boot}')
    except RuntimeError as error:
        assert 'Stale guest boot' in str(error), error
    else:
        raise AssertionError('Terminate accepted a stale boot identity')
    print('DEBUG_JOB_TERMINATE_PASS', ident)


if __name__ == '__main__':
    main()

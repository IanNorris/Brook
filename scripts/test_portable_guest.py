#!/usr/bin/env python3
"""Opt-in clean-image guest smoke test; uses only the selected VM bridge port.

Writes a small acceptance file under the guest's /data/home. Run once before
and once after a clean reboot (--reopen) to verify durable persistence. No
host filesystem or desktop input is exposed to the guest.
"""
import argparse
import json
from pathlib import Path
import secrets
import socket
import struct
import time


class Guest:
    def __init__(self, port):
        self.port = port
        self.boot = self.query('bridge epoch')['boot_id']
        if self.boot == '0' * 32:
            self.boot = self.query('bridge epoch init ' + secrets.token_hex(16))['boot_id']

    def query(self, command):
        token = str(secrets.randbits(48))
        begin, end = f'BRIDGE_BEGIN {token}\n'.encode(), f'BRIDGE_END {token}\n'.encode()
        with socket.create_connection(('127.0.0.1', self.port), timeout=10) as connection:
            connection.settimeout(20)
            connection.sendall(f'@{token} {command}\n'.encode())
            data = b''
            while end not in data and len(data) <= 2 * 1024 * 1024:
                chunk = connection.recv(65536)
                if not chunk:
                    raise RuntimeError('Guest disconnected; command completion unknown')
                data += chunk
        payload = data.split(begin, 1)[1].split(end, 1)[0]
        result = next(json.loads(line) for line in payload.decode().splitlines() if line.startswith('{'))
        for field in ('job_error', 'ui_error'):
            if field in result:
                raise RuntimeError(result[field])
        return result

    def run(self, argv, timeout=90, uid=1000):
        if uid not in (0, 1000):
            raise ValueError('Use normal user 1000 or explicit maintenance uid 0')
        ident = secrets.token_hex(16)
        payload = b'BJ01' + struct.pack('<III', uid, len(argv), 0)
        payload += b''.join(x.encode() + b'\0' for x in argv + ['/data/home'])
        self.query(f'bridge job begin {ident} {len(payload)} {self.boot}')
        for offset in range(0, len(payload), 64):
            self.query(f'bridge job put {ident} {offset} {payload[offset:offset+64].hex()} {self.boot}')
        self.query(f'bridge job commit {ident} {self.boot}')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.query(f'bridge job status {ident} {self.boot}')
            if status.get('state') in ('completed', 'failed'):
                return dict(job=ident, status=status,
                            stdout=self.query(f'bridge job read {ident} out 0 1024 {self.boot}'),
                            stderr=self.query(f'bridge job read {ident} err 0 1024 {self.boot}'))
            time.sleep(.25)
        raise TimeoutError('Job still running: ' + ident)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--reopen', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    guest = Guest(args.port)
    if args.reopen:
        script = 'set -eu; test "$(cat /data/home/portable-acceptance.txt)" = "Brook portable persistence"; /data/home/portable-hello; echo REOPEN_OK'
    else:
        script = '''set -eu
test "$(id -u)" = 1000
test -d "$HOME"
echo "Brook portable persistence" > /data/home/portable-acceptance.txt
printf '#include <stdio.h>\nint main(void){puts("TCC_OK");return 0;}\n' > /data/home/portable-hello.c
/nix/profile/bin/tcc /data/home/portable-hello.c -o /data/home/portable-hello
/data/home/portable-hello
/nix/profile/bin/python3 -c 'import ssl,json; print("PYTHON_OK")'
/nix/profile/bin/git --version
echo FRESH_OK
'''
    result = dict(boot_id=guest.boot, reopen=args.reopen, result=guest.run(['/nix/profile/bin/bash', '-c', script]))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if result['result']['status'].get('state') != 'completed' or result['result']['status'].get('wait_status') != 0:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

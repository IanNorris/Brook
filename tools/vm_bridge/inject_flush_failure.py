#!/usr/bin/env python3
"""Private, host-only FLUSH fault injection for owned disposable test VMs.

No arbitrary address/command facility. Requires the exact registered kernel ELF.
QEMU 10.2.4 gdbstub/system.c:gdbserver_start accepts a chardev string and 'none':
https://github.com/qemu/qemu/blob/v10.2.4/gdbstub/system.c
https://github.com/qemu/qemu/blob/v10.2.4/hmp-commands.hx
The Unix socket is inside a private temporary directory; no TCP listener exists.
The caller must inspect the reported state after errors; SIGKILL or loss of the
QMP connection can prevent cleanup. This is not a guest-facing bridge command.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import signal
import socket
import struct
import subprocess
import tempfile

from common import REGISTRY, QMP, load_session, session_lock

SYMBOL = 'brook_virtio_flush_failures'


def validate_numbers(slot, count):
    if type(slot) is not int or not 0 <= slot < 8:
        raise ValueError('slot must be an integer in 0..7')
    if type(count) is not int or not 0 <= count <= 8:
        raise ValueError('count must be an integer in 0..8')


def check_control(session, data):
    if data.get('provenance') != 'agent':
        raise ValueError('Fault injection requires an agent-owned test session')
    control = REGISTRY / session / 'control.json'
    if control.exists() and json.loads(control.read_text()).get('user_takeover'):
        raise ValueError('User owns control; fault injection refused')


def verify_kernel(data, elf):
    expected = data.get('kernel_sha256')
    if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-f]{64}', expected):
        raise ValueError('Session has no valid kernel hash')
    for path in (Path(elf), Path(data['kernel'])):
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('ELF hash does not match the live session kernel')
    return expected


def symbol_address(elf, nm=None):
    nm = nm or shutil.which('llvm-nm') or shutil.which('nm')
    if not nm:
        raise ValueError('llvm-nm or nm is required; use --nm PATH or the dev shell')
    result = subprocess.run([str(nm), '--defined-only', '--print-size',
                             '--format=posix', str(Path(elf).resolve())],
                            check=True, capture_output=True, text=True, timeout=15)
    rows = [line.split() for line in result.stdout.splitlines()
            if line.split() and line.split()[0] == SYMBOL]
    if len(rows) != 1 or len(rows[0]) != 4:
        raise ValueError('Expected exactly one sized fault-budget symbol')
    _, kind, address, size = rows[0]
    address, size = int(address, 16), int(size, 16)
    if kind not in ('B', 'D') or size != 32 or address % 4 or not (
            0xffffffff80000000 <= address <= 0xffffffffffffffff - 32):
        raise ValueError('Fault-budget symbol has unexpected type, size or address')
    return address


class Remote:
    """Bounded ACK-mode subset; mutation packets are never automatically retried."""
    def __init__(self, sock):
        self.sock = sock

    def byte(self):
        value = self.sock.recv(1)
        if not value:
            raise ConnectionError('GDB socket closed; completion may be unknown')
        return value

    def packet(self, command):
        payload = command.encode('ascii')
        self.sock.sendall(b'$' + payload + b'#' + f'{sum(payload) & 255:02x}'.encode())
        for _ in range(16):
            start = self.byte()
            if start == b'+':
                continue
            if start == b'-':
                raise RuntimeError('GDB rejected checksum; operation not retried')
            if start != b'$':
                raise RuntimeError('Unexpected GDB framing')
            response = bytearray()
            while True:
                value = self.byte()
                if value == b'#':
                    break
                response += value
                if len(response) > 4096:
                    raise RuntimeError('Oversized GDB packet')
            checksum = self.byte() + self.byte()
            if checksum.lower() != f'{sum(response) & 255:02x}'.encode():
                raise RuntimeError('GDB response checksum mismatch')
            self.sock.sendall(b'+')
            text = response.decode('ascii')
            if text.startswith('O') and text != 'OK':
                continue  # Optional bounded console-output packets.
            return text
        raise RuntimeError('Too many GDB acknowledgements/console packets')

    def read_u32(self, address):
        reply = self.packet(f'm{address:x},4')
        if not re.fullmatch(r'[0-9a-fA-F]{8}', reply):
            raise RuntimeError(f'GDB read failed: {reply!r}')
        return struct.unpack('<I', bytes.fromhex(reply))[0]

    def write_u32(self, address, value):
        reply = self.packet(f'M{address:x},4:{struct.pack("<I", value).hex()}')
        if reply != 'OK':
            raise RuntimeError(f'GDB write failed: {reply!r}')


def inject(session, elf, slot=1, count=1, nm=None):
    validate_numbers(slot, count)
    with session_lock(session) as data:
        check_control(session, data)
        kernel_hash = verify_kernel(data, elf)
        address = symbol_address(elf, nm) + 4 * slot
        verify_kernel(data, elf)  # reject an ELF replaced while nm was reading
        with QMP(session) as q:
            if not q.call('query-status').get('running'):
                # GDB detach resumes execution; do not momentarily run a VM
                # whose owner deliberately left it paused.
                raise ValueError('VM must already be running')
            if any(c.get('label') == 'gdb' for c in q.call('query-chardev')):
                raise ValueError('Existing GDB server; refusing to replace it')
            with tempfile.TemporaryDirectory(prefix='brook-flush-') as directory:
                path = str(Path(directory) / 'gdb.sock')
                if len(path.encode()) >= 100 or any(c in path for c in '\"\\,\n\r'):
                    raise ValueError('Unsafe/oversized private GDB socket path')
                remote = sock = None
                server_started = False
                detached = False
                cleanup_errors = []
                try:
                    q.call('stop')
                    if q.call('query-status').get('running'):
                        raise RuntimeError('QEMU did not pause')
                    current = load_session(session)
                    check_control(session, current)
                    if (current['process_identity'], current['generation'], current['kernel_sha256']) != (
                            data['process_identity'], data['generation'], kernel_hash):
                        raise ValueError('Session identity changed during injection')
                    server_started = True  # clean up even if the HMP reply is lost
                    q.hmp(f'gdbserver "unix:{path},server=on,wait=off"')
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.settimeout(3)
                    sock.connect(path)
                    remote = Remote(sock)
                    status = remote.packet('?')
                    if not status.startswith(('T', 'S')):
                        raise RuntimeError(f'Unexpected GDB stop response: {status!r}')
                    before = remote.read_u32(address)
                    if before and count:
                        raise ValueError('Budget is already armed; use count 0 to clear it')
                    check_control(session, load_session(session))
                    remote.write_u32(address, count)
                    if remote.read_u32(address) != count:
                        raise RuntimeError('Fault-budget readback mismatch')
                    if remote.packet('D') != 'OK':
                        raise RuntimeError('GDB detach failed')
                    detached = True
                finally:
                    if remote is not None and not detached:
                        try:
                            if remote.packet('D') != 'OK':
                                raise RuntimeError('GDB detach failed')
                        except Exception as exc:
                            cleanup_errors.append(str(exc))
                    if sock is not None:
                        sock.close()
                    if server_started:
                        try:
                            q.hmp('gdbserver none')
                            if any(c.get('label') == 'gdb' for c in q.call('query-chardev')):
                                raise RuntimeError('GDB listener remains installed')
                        except Exception as exc:
                            cleanup_errors.append('GDB server cleanup: ' + str(exc))
                    try:
                        q.call('cont')
                        if not q.call('query-status').get('running'):
                            raise RuntimeError('QEMU remains paused')
                    except Exception as exc:
                        cleanup_errors.append('VM resume: ' + str(exc))
                    if cleanup_errors:
                        raise RuntimeError('; '.join(cleanup_errors))
        return {'session': session, 'kernel_sha256': kernel_hash, 'symbol': SYMBOL,
                'slot': slot, 'count': count, 'previous': before,
                'address': hex(address), 'readback_verified': True, 'resumed': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--slot', type=int, required=True)
    parser.add_argument('--count', type=int, required=True)
    parser.add_argument('--nm', '--llvm-nm', dest='nm',
                        help='llvm-nm or GNU nm executable (otherwise discovered on PATH)')
    args = parser.parse_args()
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt('Interrupted; restoring VM state')
    signal.signal(signal.SIGTERM, interrupted)
    try:
        result = inject(args.session, args.elf, args.slot, args.count, args.nm)
    except (Exception, KeyboardInterrupt) as exc:
        print(json.dumps({'ok': False, 'error': str(exc),
                          'note': 'Inspect VM state and budget before retrying an uncertain operation'}))
        return 1
    print(json.dumps({'ok': True, **result}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

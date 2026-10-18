"""Network-independent guest maintenance transport; not orderly shutdown yet.

Only hello, synchronous filesystem/device flush, and retained flush status are
implemented. Callers must quiesce writers themselves. A timeout is unknown
completion; query status using the SAME boot/request IDs before further action.
No kernel-memory edits, focus-dependent input, host shell, or arbitrary guest
commands are part of this transport.
"""
import fcntl
from pathlib import Path
import re
import socket
import stat
import time

from common import REGISTRY, load_session

ID = re.compile(r'[0-9a-f]{32}\Z')


def valid_id(value):
    if not isinstance(value, str) or not ID.fullmatch(value) or value == '0' * 32:
        raise ValueError('Expected a nonzero 32-character lowercase hex identity')


def launch_args(command, session, directory, enabled=True):
    valid_id(session)
    args = list(command)
    if not isinstance(enabled, bool):
        raise ValueError('Lifecycle channel enable flag must be boolean')
    if not enabled:
        return args
    # COM1 is the existing serial log. Reserve COM2 exclusively for this
    # opt-in channel; do not silently take another configured guest device.
    if args.count('-serial') != 1 or any(a.startswith('-serial=') for a in args):
        raise ValueError('Lifecycle channel requires exactly one existing COM1 serial backend')
    if any('brook_lifecycle' in a or 'opt/brook/lifecycle' in a for a in args):
        raise ValueError('Lifecycle channel QEMU argument collision')
    endpoint = Path(directory) / 'lifecycle.sock'
    if len(str(endpoint).encode()) >= 108:
        raise ValueError('Lifecycle socket exceeds sockaddr_un path limit')
    args.extend(['-chardev', f'socket,id=brook_lifecycle,path={endpoint},server=on,wait=off',
                 '-serial', 'chardev:brook_lifecycle',
                 '-fw_cfg', f'name=opt/brook/lifecycle,string={session}'])
    return args


def exchange(session, verb, request_id, boot_id, timeout=8):
    """One bounded exchange. Never retry or infer completion on disconnect.

    For hello, boot_id is a fresh initialization proposal, never an old stored
    boot ID. For sync/status it is the expected, previously observed boot ID.
    A done response can contain a negative flush result, which must be checked.
    """
    for value in (session, request_id, boot_id):
        valid_id(value)
    if verb not in {'hello', 'sync', 'status'}:
        raise ValueError('Unsupported lifecycle transport operation')
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 0 < timeout <= 30:
        raise ValueError('Timeout must be positive and at most 30 seconds')
    deadline = time.monotonic() + timeout
    data = load_session(session)
    if data.get('lifecycle_channel', {}).get('api') != 1:
        raise RuntimeError('Selected VM has no network-independent lifecycle channel')
    directory = REGISTRY / session
    endpoint = directory / 'lifecycle.sock'
    info = endpoint.stat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != directory.stat().st_uid:
        raise RuntimeError('Lifecycle endpoint is not an owned Unix socket')
    with (directory / 'lifecycle.lock').open('a') as lock:
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Lifecycle channel is busy; nothing dispatched')
                time.sleep(.01)
        current = load_session(session)
        if current['generation'] != data['generation']:
            raise RuntimeError('VM session generation changed before dispatch')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(max(.001, deadline-time.monotonic()))
            connection.connect(str(endpoint))
            connection.sendall(f'BLC1 {session} {request_id} {boot_id} {verb}\n'.encode('ascii'))
            buffer = bytearray()
            lines = 0
            while time.monotonic() < deadline:
                connection.settimeout(max(.001, deadline-time.monotonic()))
                chunk = connection.recv(512)
                if not chunk:
                    raise ConnectionError('Lifecycle channel disconnected; completion unknown')
                buffer.extend(chunk)
                while b'\n' in buffer:
                    line, _, rest = buffer.partition(b'\n');buffer = bytearray(rest)
                    lines += 1
                    if len(line) > 159 or lines > 32:
                        raise ValueError('Lifecycle response exceeds bounds')
                    parts = line.decode('ascii').split(' ')
                    if len(parts) != 5 or parts[0] != 'BLC1':
                        raise ValueError('Malformed lifecycle response')
                    valid_id(parts[1]);valid_id(parts[2])
                    if parts[1] != request_id:
                        continue  # A reply from a disconnected prior exchange.
                    if parts[3] not in {'ok', 'done', 'stale', 'missing', 'full', 'unsupported'}:
                        raise ValueError('Unknown lifecycle response state')
                    if not re.fullmatch(r'-?(?:0|[1-9][0-9]{0,9})', parts[4]):
                        raise ValueError('Invalid lifecycle result')
                    allowed = {'hello': {'ok'}, 'sync': {'done', 'stale', 'full', 'unsupported'},
                               'status': {'done', 'stale', 'missing', 'unsupported'}}
                    if parts[3] not in allowed[verb]:
                        raise ValueError('Lifecycle state does not match the operation')
                    result = int(parts[4])
                    if not -(2**31) <= result < 2**31:
                        raise ValueError('Lifecycle result exceeds int32')
                    expected_errors = {'ok': 0, 'stale': -116, 'missing': -2, 'full': -28, 'unsupported': -95}
                    if ((parts[3] == 'done' and result > 0) or
                        (parts[3] in expected_errors and result != expected_errors[parts[3]])):
                        raise ValueError('Lifecycle state/result mismatch')
                    if verb != 'hello' and parts[2] != boot_id and parts[3] != 'stale':
                        raise RuntimeError('Lifecycle reply belongs to another guest boot')
                    return {'session': session, 'generation': data['generation'],
                            'request_id': request_id, 'guest_boot_id': parts[2],
                            'state': parts[3], 'result': result,
                            'guest_sync_completed': parts[3] == 'done' and result == 0,
                            'scope': 'filesystem/device flush only; writers must already be quiescent'}
                if len(buffer) > 159:
                    raise ValueError('Unterminated lifecycle response exceeds bound')
    raise TimeoutError('No lifecycle response; completion unknown (not retried)')

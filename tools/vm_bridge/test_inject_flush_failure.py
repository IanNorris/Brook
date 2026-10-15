"""No QEMU required: test the exact GDB wire subset and injection lifecycle."""
import contextlib
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

import inject_flush_failure as inject


def frame(payload):
    return b'$' + payload + b'#' + f'{sum(payload) & 255:02x}'.encode()


class ProtocolTests(unittest.TestCase):
    def test_fragmented_read_write_readback_detach(self):
        left, right = socket.socketpair()
        left.settimeout(2)
        right.settimeout(2)
        received, errors = [], []
        exchanges = [(b'?', b'T05thread:1;'), (b'mffffffff81000004,4', b'00000000'),
                     (b'Mffffffff81000004,4:03000000', b'OK'),
                     (b'mffffffff81000004,4', b'03000000'), (b'D', b'OK')]
        def peer():
            try:
                for expected, response in exchanges:
                    self.assertEqual(right.recv(1), b'$')
                    payload = bytearray()
                    while True:
                        byte = right.recv(1)
                        if byte == b'#': break
                        payload += byte
                    checksum = right.recv(1) + right.recv(1)
                    self.assertEqual(checksum, f'{sum(payload) & 255:02x}'.encode())
                    self.assertEqual(payload, expected)
                    received.append(bytes(payload))
                    for byte in b'+' + frame(response):
                        right.sendall(bytes([byte]))
                    self.assertEqual(right.recv(1), b'+')
            except BaseException as exc:
                errors.append(exc)
            finally:
                right.close()
        thread = threading.Thread(target=peer)
        thread.start()
        try:
            remote = inject.Remote(left)
            self.assertTrue(remote.packet('?').startswith('T'))
            self.assertEqual(remote.read_u32(0xffffffff81000004), 0)
            remote.write_u32(0xffffffff81000004, 3)
            self.assertEqual(remote.read_u32(0xffffffff81000004), 3)
            self.assertEqual(remote.packet('D'), 'OK')
        finally:
            left.close()
            thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(len(received), 5)

    def test_bad_checksum_nack_and_error_are_not_retried(self):
        for response in (b'+$OK#00', b'-', b'+' + frame(b'E14')):
            with self.subTest(response=response):
                left, right = socket.socketpair()
                left.settimeout(1)
                try:
                    right.sendall(response)
                    with self.assertRaises(RuntimeError):
                        inject.Remote(left).write_u32(0xffffffff81000000, 1)
                    sent = right.recv(1024)
                    self.assertEqual(sent.count(b'$M'), 1)
                finally:
                    left.close(); right.close()


class InjectionTests(unittest.TestCase):
    @contextlib.contextmanager
    def fixture(self, *, write_error=False, detach_error=False, connect_error=False,
                hmp_error=False, running=True, previous=0, chardev=False, takeover=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'session').mkdir()
            elf = root / 'kernel.elf'
            elf.write_bytes(b'exact-kernel')
            data = {'provenance': 'agent', 'kernel': str(elf),
                    'kernel_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                    'process_identity': '123', 'generation': 'generation'}
            if takeover:
                (root/'session'/'control.json').write_text(json.dumps({'user_takeover': True}))
            events = []
            state = {'running': running, 'gdb': chardev, 'budget': previous}
            class QMP:
                def __enter__(self): return self
                def __exit__(self, *args): pass
                def call(self, command):
                    events.append(command)
                    if command == 'query-status': return {'running': state['running']}
                    if command == 'query-chardev': return [{'label': 'gdb'}] if state['gdb'] else []
                    if command == 'stop': state['running'] = False
                    if command == 'cont': state['running'] = True
                def hmp(self, command):
                    events.append(command)
                    if command == 'gdbserver none': state['gdb'] = False
                    else:
                        self_path = command.split('unix:', 1)[1].split(',', 1)[0]
                        if not (Path(self_path).parent.stat().st_mode & 0o777) == 0o700:
                            raise AssertionError('socket directory is not private')
                        if hmp_error: raise RuntimeError('start failed')
                        state['gdb'] = True
            class Sock:
                def settimeout(self, timeout): pass
                def connect(self, path):
                    events.append('connect')
                    if connect_error: raise OSError('connect failed')
                def close(self): events.append('close')
            class Remote:
                def __init__(self, sock): pass
                def packet(self, command):
                    events.append(command)
                    if command == '?': return 'T05thread:1;'
                    if detach_error: raise RuntimeError('detach failed')
                    state['running'] = True
                    return 'OK'
                def read_u32(self, address):
                    events.append(('read', address))
                    return state['budget']
                def write_u32(self, address, value):
                    events.append(('write', address, value))
                    if write_error: raise RuntimeError('write failed')
                    state['budget'] = value
            @contextlib.contextmanager
            def lock(session): yield data
            with patch.object(inject, 'REGISTRY', root), patch.object(inject, 'session_lock', lock), \
                 patch.object(inject, 'load_session', return_value=data), \
                 patch.object(inject, 'symbol_address', return_value=0xffffffff81000000), \
                 patch.object(inject, 'QMP', lambda _: QMP()), \
                 patch.object(inject.socket, 'socket', lambda *args: Sock()), \
                 patch.object(inject, 'Remote', Remote):
                yield elf, data, events, state

    def test_success_exact_slot_and_cleanup(self):
        with self.fixture() as (elf, data, events, state):
            result = inject.inject('session', elf, slot=2, count=3)
            self.assertEqual(result['address'], '0xffffffff81000008')
            self.assertIn(('write', 0xffffffff81000008, 3), events)
            self.assertTrue(state['running'])
            self.assertFalse(state['gdb'])
            self.assertLess(events.index('D'), events.index('gdbserver none'))

    def test_each_failure_restores_running_and_removes_listener(self):
        for option in ('write_error', 'detach_error', 'connect_error', 'hmp_error'):
            with self.subTest(option=option), self.fixture(**{option: True}) as (elf, _, events, state):
                with self.assertRaises((RuntimeError, OSError)):
                    inject.inject('session', elf)
                self.assertTrue(state['running'])
                self.assertFalse(state['gdb'])
                self.assertIn('gdbserver none', events)

    def test_preflight_refusals_do_not_pause(self):
        for option in ({'running': False}, {'chardev': True}, {'takeover': True}):
            with self.subTest(option=option), self.fixture(**option) as (elf, _, events, _):
                with self.assertRaises(ValueError): inject.inject('session', elf)
                self.assertNotIn('stop', events)
        for change in ('provenance', 'hash'):
            with self.fixture() as (elf, data, events, _):
                if change == 'provenance': data['provenance'] = 'user'
                else: data['kernel_sha256'] = '0' * 64
                with self.assertRaises(ValueError): inject.inject('session', elf)
                self.assertNotIn('stop', events)

    def test_armed_budget_is_not_overwritten_but_can_be_cleared(self):
        with self.fixture(previous=2) as (elf, _, events, state):
            with self.assertRaisesRegex(ValueError, 'already armed'):
                inject.inject('session', elf)
            self.assertFalse(any(isinstance(x, tuple) and x[0] == 'write' for x in events))
            self.assertTrue(state['running'])
        with self.fixture(previous=2) as (elf, _, events, state):
            inject.inject('session', elf, count=0)
            self.assertEqual(state['budget'], 0)

    def test_bounds(self):
        for slot, count in ((-1, 1), (8, 1), (1, -1), (1, 9), (True, 1)):
            with self.assertRaises(ValueError): inject.validate_numbers(slot, count)

    def test_takeover_after_pause_refuses_write_and_resumes(self):
        with self.fixture() as (elf, data, events, state):
            calls = 0
            def changed(session):
                nonlocal calls
                calls += 1
                if calls == 2:
                    (inject.REGISTRY/session/'control.json').write_text(
                        json.dumps({'user_takeover': True}))
                return data
            with patch.object(inject, 'load_session', side_effect=changed):
                with self.assertRaisesRegex(ValueError, 'User owns control'):
                    inject.inject('session', elf)
            self.assertFalse(any(isinstance(x, tuple) and x[0] == 'write' for x in events))
            self.assertTrue(state['running'])
            self.assertFalse(state['gdb'])

    def test_symbol_requires_exact_global_array(self):
        def nm(text): return subprocess.CompletedProcess([], 0, stdout=text, stderr='')
        valid = 'brook_virtio_flush_failures B ffffffff81000000 20\n'
        with patch.object(inject.subprocess, 'run', return_value=nm(valid)):
            self.assertEqual(inject.symbol_address('/tmp/kernel', '/usr/bin/llvm-nm'), 0xffffffff81000000)
        for text in ('', valid * 2, valid.replace(' 20', ' 4'), valid.replace(' B ', ' T '),
                     valid.replace('ffffffff81000000', '1000')):
            with patch.object(inject.subprocess, 'run', return_value=nm(text)):
                with self.assertRaises(ValueError): inject.symbol_address('/tmp/kernel', '/usr/bin/llvm-nm')

    def test_nm_discovery_prefers_llvm_and_falls_back_to_gnu(self):
        output = subprocess.CompletedProcess([], 0, stdout=
            'brook_virtio_flush_failures B ffffffff81000000 20\n', stderr='')
        for available, expected in (({'llvm-nm': '/bin/llvm-nm', 'nm': '/bin/nm'}, '/bin/llvm-nm'),
                                    ({'nm': '/bin/nm'}, '/bin/nm')):
            with self.subTest(available=available), \
                 patch.object(inject.shutil, 'which', side_effect=available.get), \
                 patch.object(inject.subprocess, 'run', return_value=output) as run:
                self.assertEqual(inject.symbol_address('/tmp/kernel'), 0xffffffff81000000)
                self.assertEqual(run.call_args.args[0][0], expected)
        with patch.object(inject.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(ValueError, 'llvm-nm or nm'):
                inject.symbol_address('/tmp/kernel')


if __name__ == '__main__': unittest.main()

#!/usr/bin/env python3
"""Host regression checks for v3 promotion and named installer staging."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import hardware_promote_slot
import stage_ssd_install
from binary_bridge import receiver_key
from bridge_v3 import create_operator_key


class Bridge:
    def __init__(self, data):
        self.data = data
        self.tx = None
        self.argv = None
        self.command = {'state': 'running', 'stdout': {'text': 'HWUPDATE_LISTENING'}}
        self.final = None

    def load_session(self, session):
        return self.data

    def doctor(self, session):
        return {'probe_stage': 'verified', 'reachable': True, 'identity_matches': True}

    def guest_run(self, session, transaction, argv, **options):
        self.tx = transaction
        self.argv = argv
        return self.command

    def job(self, session, transaction, **options):
        if isinstance(self.final, dict):
            return self.final
        return self.final(self.tx)


class Workflows(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.operator = self.root/'operator.seed'
        self.public = create_operator_key(self.operator)
        self.missing_legacy = self.root/'legacy-deleted'
        self.guest_boot = 'a'*32
        self.bridge = Bridge(dict(host='127.0.0.1', guest_boot_id=self.guest_boot, auth_mode='v3',
                                  operator_key_file=str(self.operator)))

    def test_receiver_refuses_old_registry_before_reading_key(self):
        bogus = self.root/'legacy'
        bogus.write_text('1'*32)
        for mode_data in ({}, {'auth_mode': None}, {'auth_mode': 'unknown'}, {'auth_mode': 'v3'}):
            with self.subTest(mode_data=mode_data), self.assertRaisesRegex(ValueError, 'reattach explicitly'):
                receiver_key(mode_data, bogus)
        self.assertEqual(receiver_key({'auth_mode': 'legacy'}, bogus), '1'*32)
        with self.assertRaisesRegex(ValueError, 'requires a legacy key file'):
            receiver_key({'auth_mode': 'legacy'}, None)

    def test_promote_v3_uses_operator_and_slot_aware_boot_helper(self):
        kernel = self.root/'kernel'; kernel.write_bytes(b'kernel' * 24)
        initrd = self.root/'initrd'; initrd.write_bytes(b'initrd' * 96)
        self.bridge.final = lambda tx: {
            'state': 'completed', 'exit': {'code': 0},
            'stdout': {'text': f'HWUPDATE_PROMOTED generation=43 slot=0 transaction={tx} reboot_required=1'}
        }
        reply = {'state': 'promoted', 'generation': 43, 'slot': 0}
        with patch.object(hardware_promote_slot, 'promote', return_value=reply) as send:
            result = hardware_promote_slot.run(self.bridge, 'session', self.missing_legacy,
                                                0, kernel, initrd, self.root/'output',
                                                expected_generation=43, activate=True)
        self.assertEqual(self.bridge.argv, ['/boot/HWUPSYS.ELF', self.guest_boot, self.bridge.tx,
                                            '--require-slot-aware'])
        _, _, key, boot, tx, _, *_ = send.call_args.args
        self.assertEqual(bytes(key.verify_key), self.public)
        self.assertEqual((boot, tx), (self.guest_boot, self.bridge.tx))
        self.assertEqual(send.call_args.kwargs['expected_generation'], 43)
        self.assertEqual(result['state'], 'promoted')
        self.assertEqual(result['auth_mode'], 'v3')

    def test_promotion_refuses_legacy_receiver_without_opening_data_socket(self):
        payload = self.root/'payload'; payload.write_bytes(b'x'*512)
        self.bridge.command = {'state': 'completed', 'exit': {'code': 2}, 'stdout': {'text': ''}}
        output = self.root/'output'
        with patch.object(hardware_promote_slot, 'promote') as send, self.assertRaisesRegex(
                RuntimeError, 'lacks running-slot support'):
            hardware_promote_slot.run(self.bridge, 'session', None, 0, payload, payload, output)
        send.assert_not_called()
        self.assertEqual(json.loads((output/'receipt.json').read_text())['state'], 'needs_inspection')

    def stage_args(self):
        return ['--session', 'session', '--package', str(self.root), '--output', str(self.root/'output'),
                '--file', 'INSTHASH.BIN']

    def test_stage_v3_uses_installer_receiver_and_operator_key(self):
        record = self.root/'INSTHASH.BIN'
        record.write_bytes(b'synthetic installer record')
        digest = hashlib.sha256(record.read_bytes()).hexdigest()
        transfer = {'guest_path': '/media/usb/INSTHASH.BIN', 'sha256': digest, 'bytes': record.stat().st_size}
        states = [{'state': 'running', 'stdout': {'text': 'HWBULK_LISTENING'}},
                  {'state': 'completed', 'exit': {'code': 0}}]
        self.bridge.final = Mock(side_effect=states)
        manifest = {'segments': [], 'install_record_sha256': digest}
        with patch.object(stage_ssd_install, 'Hardware', return_value=self.bridge), patch.object(
                stage_ssd_install, 'verify_package', return_value=manifest), patch.object(
                stage_ssd_install, 'upload', return_value=transfer) as send, contextlib.redirect_stdout(io.StringIO()):
            stage_ssd_install.main([*self.stage_args(), '--key-file', str(self.missing_legacy)])
        self.assertEqual(self.bridge.argv, ['/boot/HWSTAGE.ELF', self.guest_boot, self.bridge.tx,
                                            'put', 'INSTHASH.BIN'])
        self.assertEqual(bytes(send.call_args.args[2].verify_key), self.public)
        self.assertEqual(json.loads((self.root/'output/receipt.json').read_text())['state'], 'completed')

    def test_stage_v3_rejects_the_wrong_helper_before_dispatch(self):
        with patch.object(stage_ssd_install, 'Hardware', return_value=self.bridge), patch.object(
                stage_ssd_install, 'verify_package', return_value={'segments': []}),                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            stage_ssd_install.main([*self.stage_args(), '--helper', '/boot/HWBULK.ELF'])
        self.assertIsNone(self.bridge.argv)
        self.assertFalse((self.root/'output').exists())


if __name__ == '__main__':
    unittest.main()

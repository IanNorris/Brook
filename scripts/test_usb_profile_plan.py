"""Host policy tests for signed USB plan and closure-diff admission."""
import base64
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import Mock, patch

from dell_deploy import usb_store_diff
from usb_profile_plan import checked_plan


def stream(text):
    return {'stdout': {'text': text, 'next_offset': len(text), 'eof': True}, 'exit': {'code': 0}}


class UsbPlanTests(unittest.TestCase):
    def test_signature_key_and_sequence(self):
        with TemporaryDirectory() as scratch:
            base = Path(scratch)
            for prefix in ('good', 'wrong'):
                subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519',
                                '-out', base / (prefix + '.key')], check=True)
                subprocess.run(['openssl', 'pkey', '-in', base / (prefix + '.key'),
                                '-pubout', '-out', base / (prefix + '.pub')], check=True)
            name = '0' * 32 + '-desktop'
            bins = base / 'store' / name / 'bin'
            bins.mkdir(parents=True)
            for binary in ('bash', 'brook-chromium', 'brook-gallery', 'hardware-dbus',
                           'hardware-message', 'waylandd', 'brook-stage-profile'):
                executable = bins / binary
                executable.write_bytes(b'fixture')
                executable.chmod(0o755)
            zero = base64.b64encode(bytes(32)).decode()
            plan = {
                'format': 3, 'roots': [name], 'packages': [],
                'cache': {'brook': None, 'mirrorUpstream': False},
                'closure': {name: {'narHash': 'sha256-' + zero, 'narSize': 16,
                                   'source': 'brook', 'references': [], 'storeInodes': 1}},
                'profile': {'root': name, 'launcherIds': [], 'layoutSha256': '0' * 64,
                            'space': dict.fromkeys(('links', 'directories', 'externalLinks',
                                                   'externalLinkBytes', 'directoryBytes'), 0)},
            }
            document = base / 'stage-plan.json'
            signature = base / 'stage-plan.sig'

            def sign(**changes):
                document.write_text(json.dumps({**plan, **changes}))
                subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', base / 'good.key',
                                '-in', document, '-out', signature], check=True)

            with patch('usb_profile_plan.STORE', base / 'store'):
                for invalid in (0, -1, True, 1 << 63):
                    sign(sequence=invalid)
                    with self.assertRaisesRegex(ValueError, 'signed sequence'):
                        checked_plan(base, base / 'good.pub')
                sign(sequence=4)
                self.assertEqual(checked_plan(base, base / 'good.pub')[0]['sequence'], 4)
                with self.assertRaises(subprocess.CalledProcessError):
                    checked_plan(base, base / 'wrong.pub')
                document.write_bytes(document.read_bytes() + b' ')
                with self.assertRaises(subprocess.CalledProcessError):
                    checked_plan(base, base / 'good.pub')

    def test_complete_and_incomplete_store_inventory(self):
        closure = {str(n) * 32 + '-name': {} for n in range(3)}
        a, b, c = closure
        bridge, journal = Mock(), Mock()
        cap = {'selected_store': {'transfer_shares_space': True, 'store_names': [a],
                                  'store_inventory_complete': True}}
        self.assertEqual(usb_store_diff(bridge, journal, 'session', cap, closure), {b, c})
        bridge.guest_run.assert_not_called()
        cap['selected_store']['store_inventory_complete'] = False
        journal.wait_job.return_value = stream(b + '\n')
        self.assertEqual(usb_store_diff(bridge, journal, 'session', cap, closure), {c})
        argv = bridge.guest_run.call_args.args[2]
        self.assertEqual(argv[-2:], [b, c])
        journal.wait_job.return_value = stream('outside-plan\n')
        with self.assertRaisesRegex(ValueError, 'Invalid USB profile store inventory'):
            usb_store_diff(bridge, journal, 'session', cap, closure)
        cap['selected_store']['transfer_shares_space'] = False
        with self.assertRaisesRegex(ValueError, 'share'):
            usb_store_diff(bridge, journal, 'session', cap, closure)


if __name__ == '__main__':
    unittest.main()

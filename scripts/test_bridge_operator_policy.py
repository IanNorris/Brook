"""Offline tests for the evidence required before deploying a v3 pair."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from bridge_operator_policy import verify_receiver_evidence


class ReceiverEvidenceTest(unittest.TestCase):
    def setUp(self):
        directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.verdict_path = directory / 'verdict.json'
        run = directory / 'run-1'
        run.mkdir()
        self.evidence_path = run / 'bridge-v3-qa.json'
        self.operator = bytes.fromhex('ab' * 32)
        helpers = {'HWUPSYS.ELF': '1' * 64}
        self.manifest = {
            'kernel_sha256': '2' * 64,
            'initrd_sha256': '3' * 64,
            'bridge_v3': {'policy_sha256': '4' * 64, 'helpers_sha256': helpers},
        }
        self.evidence = {
            'package_kernel_sha256': self.manifest['kernel_sha256'],
            'package_initrd_sha256': self.manifest['initrd_sha256'],
            'operator_public_key': self.operator.hex(),
            'policy_sha256': self.manifest['bridge_v3']['policy_sha256'],
            'packaged_helpers_sha256': helpers.copy(),
            'receivers': {
                'command': True,
                'bulk': True,
                'update': {'helper': '/boot/HWUPSYS.ELF', 'authenticated_end': True,
                           'safe_destination_refusal': 11},
                'store': {'helper': '/boot/HWSTORE.ELF', 'authenticated_end': True,
                          'safe_destination_refusal': 23},
            },
        }
        self.attempt = {'passed': True, 'output': 'run-1',
                        'bridge_v3_evidence': self.evidence_path.name}
        self.verdict = {'passed': True, 'attempts': [self.attempt]}
        self.record_evidence()

    def record_evidence(self):
        raw = json.dumps(self.evidence).encode()
        self.evidence_path.write_bytes(raw)
        self.attempt['bridge_v3_evidence_sha256'] = hashlib.sha256(raw).hexdigest()

    def verify(self):
        verify_receiver_evidence(self.verdict_path, self.verdict, self.manifest, self.operator)

    def test_exact_pair_and_operator_are_accepted(self):
        self.verify()

    def test_ordinary_verdict_is_refused(self):
        del self.attempt['bridge_v3_evidence']
        with self.assertRaisesRegex(ValueError, 'passing --bridge-v3-qa'):
            self.verify()

    def test_other_operator_is_refused(self):
        self.operator = bytes.fromhex('cd' * 32)
        with self.assertRaisesRegex(ValueError, 'different pair or operator'):
            self.verify()

    def test_other_package_is_refused(self):
        self.manifest['initrd_sha256'] = '5' * 64
        with self.assertRaisesRegex(ValueError, 'different pair or operator'):
            self.verify()

    def test_missing_receiver_is_refused(self):
        del self.evidence['receivers']['store']
        self.record_evidence()
        with self.assertRaisesRegex(ValueError, 'must cover command, bulk, update and store'):
            self.verify()

    def test_changed_evidence_is_refused(self):
        self.evidence_path.write_bytes(self.evidence_path.read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'changed after the check'):
            self.verify()


if __name__ == '__main__':
    unittest.main()

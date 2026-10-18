"""Exercise the experiment entrypoint with a harmless substituted QEMU launcher."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExperimentArgumentsTests(unittest.TestCase):
    def test_paused_boot_and_argument_boundaries_reach_qemu(self):
        with tempfile.TemporaryDirectory(prefix='brook experiment ') as tmp:
            root = Path(tmp)
            scripts = root / 'scripts'
            scripts.mkdir()
            shutil.copy2(ROOT / 'scripts/run_experiment.sh', scripts / 'run_experiment.sh')
            stub = scripts / 'run-qemu.sh'
            stub.write_text('#!/usr/bin/env python3\nimport sys,os,json\n'
                            'from pathlib import Path\n'
                            'Path(os.environ["BROOK_ARGUMENT_RECEIPT"]).write_text(json.dumps(sys.argv[1:]))\n')
            stub.chmod(0o755)
            disks = root / 'source images'
            disks.mkdir()
            for name in ('boot', 'data', 'nix'):
                (disks / (name + '.img')).write_bytes(b'fixture')
            receipt = root / 'receipt.json'
            env = dict(os.environ, BROOK_EXPERIMENT_DISKS=str(disks),
                       BROOK_DISK_IMG=str(disks / 'boot.img'),
                       BROOK_EXPERIMENT_RUN=str(root / 'private run'),
                       BROOK_ARGUMENT_RECEIPT=str(receipt), BROOK_INSTANCE='81',
                       BROOK_EXPERIMENT_DISPLAY='headless')
            result = subprocess.run(['bash', str(scripts / 'run_experiment.sh'),
                                     'desktop', '-S', '-name', 'Brook boot recording'],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            args = json.loads(receipt.read_text())
            self.assertEqual(args[-3:], ['-S', '-name', 'Brook boot recording'])
            self.assertIn('experiment_desktop', args)
            self.assertEqual(args.count('-S'), 1)
            receipt.unlink()
            result = subprocess.run(['bash', str(scripts / 'run_experiment.sh')],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(receipt.read_text())[-2:], ['--script', 'experiment_desktop'])
            receipt.unlink()
            result = subprocess.run(['bash', str(scripts / 'run_experiment.sh'), 'invalid', '-S'],
                                    env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(receipt.exists())


if __name__ == '__main__':
    unittest.main()

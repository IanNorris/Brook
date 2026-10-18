import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import bootstrap_acceptance as acceptance
class LauncherExit(unittest.TestCase):
    def test_real_owned_wrapper_exit_zero_is_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            child=subprocess.Popen([sys.executable,'-c','raise SystemExit(0)'])
            row=acceptance.wait_launcher(child,Path(tmp),'owned-session')
            self.assertEqual(row['exit_code'],0);self.assertEqual(row['pid'],child.pid)
            self.assertEqual(json.loads((Path(tmp)/'launcher-exit.json').read_text())['session'],'owned-session')
    def test_nonzero_wrapper_exit_cannot_be_hidden_by_qemu_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            child=subprocess.Popen([sys.executable,'-c','raise SystemExit(23)'])
            with self.assertRaisesRegex(RuntimeError,'VM launcher exited 23'):
                acceptance.wait_launcher(child,Path(tmp),'owned-session')
            self.assertEqual(json.loads((Path(tmp)/'launcher-exit.json').read_text())['exit_code'],23)
if __name__=='__main__':unittest.main()

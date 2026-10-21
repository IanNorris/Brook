#!/usr/bin/env python3
"""Missing generated inputs must fail before changing private review artifacts.

Run in nix-shell (stage_review_kernel imports pyelftools). No real disks, VM,
firmware or external FAT command is used by these negative preflight cases.
"""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RequiredInputs(unittest.TestCase):
    def case(self, empty):
        directory = ROOT/'build/reviews'
        directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='stage-preflight-', dir=directory) as temporary:
            root = Path(temporary)
            review, build = root/'review', root/'build'
            (review/'artifacts/kernel/drivers').mkdir(parents=True)
            build.mkdir()
            (review/'boot.img').write_bytes(b'private placeholder, must not be opened as FAT')
            (review/'artifacts/kernel/BROOK.elf').write_bytes(b'existing kernel')
            (review/'artifacts/kernel/drivers/keep.mod').write_bytes(b'existing module')
            (build/'BROOK.elf').write_bytes(b'candidate input, must not reach ELF/module/FAT staging')
            if empty:
                (build/'ksym_addr_table.cpp').touch()
            before = {str(p.relative_to(review)): p.read_bytes()
                      for p in review.rglob('*') if p.is_file()}
            result = subprocess.run([
                sys.executable, str(ROOT/'scripts/stage_review_kernel.py'),
                '--review', str(review), '--build', str(build), '--module-abi', '34',
                '--expected-kernel-sha256', hashlib.sha256((build/'BROOK.elf').read_bytes()).hexdigest(),
            ], capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Incomplete build: missing or empty ksym_addr_table.cpp; nothing staged', result.stderr)
            after = {str(p.relative_to(review)): p.read_bytes()
                     for p in review.rglob('*') if p.is_file()}
            self.assertEqual(before, after)
            self.assertFalse((review/'staged-driver-verification').exists())

    def test_missing_symbols(self):
        self.case(False)

    def test_empty_symbols(self):
        self.case(True)


if __name__ == '__main__':
    unittest.main()

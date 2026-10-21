#!/usr/bin/env python3
"""Exercise the actual audio-option block without staging or starting a VM."""
import os
from pathlib import Path
import subprocess
import unittest


class AudioOptionsTests(unittest.TestCase):
    def run_options(self, value=None, no_audio=0):
        source = Path(__file__).with_name('run-qemu.sh').read_text()
        start = source.index('HDA_USE_TIMER=')
        end = source.index('\n# Compositor mode:', start)
        # Execute production validation and option construction, not a copy.
        body = source[start:end] + '\nprintf "%s\\n" "$AUDIO_OPTS"\n'
        env = {k:v for k,v in os.environ.items() if not k.startswith('BROOK_AUDIO') and
               k != 'BROOK_HDA_USE_TIMER'}
        env['NO_AUDIO'] = str(no_audio)
        if value is not None:
            env['BROOK_AUDIO_TIMER_US'] = value
        return subprocess.run(['bash', '-eu', '-c', body], env=env,
                              capture_output=True, text=True, timeout=5)

    def test_normal_default_is_unchanged(self):
        result = self.run_options()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('out.buffer-length=20000,timer-period=5000', result.stdout)
        self.assertIn('use-timer=off', result.stdout)

    def test_candidate_and_decimal_boundaries(self):
        for value, expected in [('1000', '1000'), ('0001000', '1000'), ('1000000', '1000000')]:
            with self.subTest(value=value):
                result = self.run_options(value)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('timer-period='+expected, result.stdout)

    def test_invalid_values_do_not_reach_qemu(self):
        for value in ('0', '999', '-1000', '1000001', '100000000000000000000', '1e3', '$(false)', '1000,foo=bar'):
            with self.subTest(value=value):
                result = self.run_options(value)
                self.assertEqual(result.returncode, 2)
                self.assertIn('BROOK_AUDIO_TIMER_US', result.stderr)
                self.assertEqual(result.stdout, '')

    def test_no_audio_retains_none_backend(self):
        result = self.run_options('1000', no_audio=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '-audiodev none,id=hda0')


if __name__ == '__main__':
    unittest.main()

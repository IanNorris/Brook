#!/usr/bin/env python3
"""The profiler converter must not silently accept a truncated capture."""
import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from profiler_to_speedscope import parse_serial_log


class ProfileCompletenessTest(unittest.TestCase):
    def parse(self, content, allow_partial=False):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'profile.txt'
            path.write_text(content)
            return parse_serial_log(path, allow_partial)

    def test_complete_capture(self):
        data = self.parse('PROF_BEGIN 1 42\nP 1 0001 0 0 ffffffff80001000\nPROF_END 1 0\n')
        self.assertEqual((data[0], data[1], len(data[2]), data[4]), (1, 42, 1, 0))

    def test_truncated_rejected(self):
        with self.assertRaisesRegex(ValueError, 'PROF_END'):
            self.parse('PROF_BEGIN 1 42\nP 1 0001 0 0 ffffffff80001000\n')

    def test_partial_requires_explicit_opt_in_and_warns(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            data = self.parse('PROF_BEGIN 1 42\nP 1 0001 0 0 ffffffff80001000\n', True)
        self.assertEqual(len(data[2]), 1)
        self.assertIn('incomplete', stderr.getvalue())

    def test_incomplete_footer_rejected(self):
        for footer in ('PROF_END', 'PROF_END 1', 'PROF_ENDx', 'PROF_END 1 0'):
            with self.subTest(footer=footer), self.assertRaisesRegex(ValueError, 'PROF_END'):
                self.parse('PROF_BEGIN 1 42\nP 1 0001 0 0 ffffffff80001000\n' + footer)

    def test_interrupted_capture_before_later_capture_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'PROF_END'):
            self.parse('PROF_BEGIN 1 1\nPROF_BEGIN 1 2\nPROF_END 0 0\n')


if __name__ == '__main__':
    unittest.main()

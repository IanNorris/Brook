#!/usr/bin/env python3
"""The compatibility copy changes one bounded data literal, never ELF code."""
import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parent / 'workloads'))
from steamrt_provider_cache import relocate_loader_cache

OLD = '/nix/store/aaaaaaaa-glibc-2.42/etc/ld.so.cache'


def fixture():
    data = bytearray(1024)
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 0,
                     ident, 3, 62, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0, 0, 256, 256, 4096)
    struct.pack_into('<IIQQQQQQ', data, 120, 1, 4, 256, 256, 256, 512, 512, 4096)
    data[300:300+len(OLD)+1] = OLD.encode() + b'\0'
    help_text = b'Help text: ' + OLD.encode() + b'\nDo not truncate this\0'
    data[500:500+len(help_text)] = help_text
    return bytes(data)


class LoaderCache(unittest.TestCase):
    def test_only_standalone_readonly_literal_changes(self):
        before = fixture()
        after, manifest = relocate_loader_cache(before, OLD)
        end = 300 + len(OLD) + 1
        self.assertEqual(len(after), len(before))
        self.assertEqual(after[:300], before[:300])
        self.assertEqual(after[end:], before[end:])
        self.assertEqual(after[300:end], b'/etc/ld.so.cache' + bytes(end-300-16))
        self.assertEqual(manifest['offset'], 300)
        self.assertEqual(manifest['source_sha256'], hashlib.sha256(before).hexdigest())
        self.assertEqual(manifest['output_sha256'], hashlib.sha256(after).hexdigest())
        self.assertTrue(manifest['build_id_unchanged'])

    def test_missing_or_duplicate_literal_rejected(self):
        with self.assertRaises(ValueError):
            relocate_loader_cache(fixture().replace(OLD.encode(), b'x'*len(OLD)), OLD)
        data = bytearray(fixture())
        data[700:700+len(OLD)+1] = OLD.encode() + b'\0'
        with self.assertRaises(ValueError):
            relocate_loader_cache(bytes(data), OLD)

    def test_partial_code_overlap_rejected(self):
        data = bytearray(fixture())
        # A second R-only segment still covers the literal, but the first
        # executable segment now overlaps its first byte.
        struct.pack_into('<QQ', data, 96, 301, 301)
        with self.assertRaises(ValueError):
            relocate_loader_cache(bytes(data), OLD)

    def test_code_writable_and_unbacked_literal_rejected(self):
        for flags in (5, 6):
            data = bytearray(fixture())
            struct.pack_into('<I', data, 124, flags)
            with self.assertRaises(ValueError):
                relocate_loader_cache(bytes(data), OLD)
        data = bytearray(fixture())
        struct.pack_into('<I', data, 120, 4)  # PT_NOTE, not PT_LOAD
        with self.assertRaises(ValueError):
            relocate_loader_cache(bytes(data), OLD)

    def test_malformed_headers_and_paths_rejected(self):
        mutations = [(4, b'\x01'), (18, b'\x03\x00'), (32, struct.pack('<Q', 4096)),
                     (152, struct.pack('<Q', 4096))]
        for at, replacement in mutations:
            data = bytearray(fixture());data[at:at+len(replacement)] = replacement
            with self.assertRaises(ValueError):
                relocate_loader_cache(bytes(data), OLD)
        for path in ('relative', '/tmp/ld.so.cache'):
            with self.assertRaises(ValueError):
                relocate_loader_cache(fixture(), path)
        with self.assertRaises(ValueError):
            relocate_loader_cache(fixture(), OLD, '/tmp/cache')


if __name__ == '__main__':
    unittest.main()

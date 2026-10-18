#!/usr/bin/env python3
"""Exercise malformed build inputs against the actual linked helper ELF.

Usage: python3 scripts/test_bake_vdso.py build/release/kernel/brook-vdso.so
"""
from pathlib import Path
import struct
import sys
import unittest
from bake_vdso import code_page

ELF = Path(sys.argv.pop(1)).read_bytes()


class VdsoImageTests(unittest.TestCase):
    def changed(self, offset, fmt, value):
        blob = bytearray(ELF)
        struct.pack_into(fmt, blob, offset, value)
        return blob

    def test_section_tables_are_omitted(self):
        page = code_page(ELF + b"SECRET_TRAILING_BYTES")
        self.assertEqual(len(page), 4096)
        self.assertEqual(page[40:48] + page[58:64], bytes(14))
        self.assertNotIn(b"SECRET_TRAILING_BYTES", page)
        filesz = struct.unpack_from("<Q", ELF, 64 + 32)[0]
        self.assertEqual(page[filesz:], bytes(4096-filesz))

    def test_reject_truncation(self):
        for length in (0, 63, 231):
            with self.assertRaises(ValueError):
                code_page(ELF[:length])

    def test_reject_wrong_machine(self):
        with self.assertRaises(ValueError):
            code_page(self.changed(18, "<H", 183))

    def test_reject_writable_load(self):
        with self.assertRaises(ValueError):
            code_page(self.changed(64 + 4, "<I", 7))

    def test_reject_oversized_code(self):
        with self.assertRaises(ValueError):
            code_page(self.changed(64 + 32, "<Q", 4097))

    def test_reject_missing_clock_extent(self):
        with self.assertRaises(ValueError):
            code_page(self.changed(64 + 40, "<Q", 4096))

    def test_reject_external_dependencies(self):
        dynamic = struct.unpack_from("<Q", ELF, 64 + 56 + 8)[0]
        with self.assertRaises(ValueError):
            code_page(self.changed(dynamic, "<Q", 1))  # DT_NEEDED

    def test_reject_lookup_outside_code(self):
        dynamic = struct.unpack_from("<Q", ELF, 64 + 56 + 8)[0]
        offset = dynamic
        while struct.unpack_from("<Q", ELF, offset)[0] != 6:  # DT_SYMTAB
            offset += 16
        with self.assertRaises(ValueError):
            code_page(self.changed(offset + 8, "<Q", 4096))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
import struct
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
from hardware_recovery_fsck import embed_checker, validate_checker


def elf(program_type=1):
    data = bytearray(128)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HH', data, 16, 2, 62)
    struct.pack_into('<I', data, 20, 1)
    struct.pack_into('<Q', data, 24, 0x400040)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HHH', data, 52, 64, 56, 1)
    struct.pack_into('<I', data, 64, program_type)
    struct.pack_into('<I', data, 68, 5)
    struct.pack_into('<Q', data, 72, 0)
    struct.pack_into('<Q', data, 80, 0x400000)
    struct.pack_into('<Q', data, 96, len(data))
    struct.pack_into('<Q', data, 104, len(data))
    return data


class CheckerValidation(unittest.TestCase):
    def test_static_and_unsafe_headers(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'checker'
            p.write_bytes(elf())
            self.assertEqual(validate_checker(p)['bytes'], 128)
            for program_type in (2, 3):
                p.write_bytes(elf(program_type))
                with self.assertRaises(ValueError): validate_checker(p)
            malformed = elf(); struct.pack_into('<Q', malformed, 32, 1000)
            p.write_bytes(malformed)
            with self.assertRaises(ValueError): validate_checker(p)
            malformed = elf(); struct.pack_into('<Q', malformed, 96, 129)
            p.write_bytes(malformed)
            with self.assertRaises(ValueError): validate_checker(p)
            malformed = elf(); struct.pack_into('<Q', malformed, 104, 1)
            p.write_bytes(malformed)
            with self.assertRaises(ValueError): validate_checker(p)
            malformed = elf(); struct.pack_into('<Q', malformed, 24, 0)
            p.write_bytes(malformed)
            with self.assertRaises(ValueError): validate_checker(p)
            p.write_bytes(b'not an executable')
            with self.assertRaises(ValueError): validate_checker(p)

    def test_embedding_requires_matching_readback(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'checker'; p.write_bytes(elf())
            with patch('hardware_recovery_fsck.subprocess.run'), patch('hardware_recovery_fsck.subprocess.check_output', return_value=b'wrong'):
                with self.assertRaises(RuntimeError): embed_checker(Path(d) / 'initrd', p)


if __name__ == '__main__': unittest.main()

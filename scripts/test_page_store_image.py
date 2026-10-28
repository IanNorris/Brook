#!/usr/bin/env python3
import contextlib
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import page_store_image as image

class PageStoreImageTests(unittest.TestCase):
    def test_create_validate_and_never_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "swap.img"
            with patch.object(sys, "argv", ["tool", "create", str(path), "--mib", "1"]):
                with contextlib.redirect_stdout(io.StringIO()):
                    image.main()
                self.assertEqual(image.inspect(path)["slots"], 256)
                before = path.read_bytes()
                with self.assertRaises(FileExistsError):
                    image.main()
                self.assertEqual(path.read_bytes(), before)

    def test_bad_headers_and_short_capacity(self):
        prefix = struct.pack("<8sIIQQ16s", image.MAGIC, 1, 4096, 1, 4096, b"x" * 16)
        header = prefix + struct.pack("<Q", image.checksum(prefix))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "swap.img"
            path.write_bytes(header + bytes(8192-len(header)))
            self.assertEqual(image.inspect(path)["slots"], 1)
            for position in [0, 8, 12, 16, 24, 32, 48, 56, 4095]:
                damaged = bytearray(header + bytes(8192-len(header)))
                damaged[position] ^= 1
                path.write_bytes(damaged)
                with self.assertRaises(ValueError):
                    image.inspect(path)
            path.write_bytes(header + bytes(4096-len(header)))
            with self.assertRaises(ValueError):
                image.inspect(path)
            path.write_bytes(header)
            with self.assertRaises(ValueError):
                image.inspect(path)

if __name__ == "__main__":
    unittest.main()

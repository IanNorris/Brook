"""Check the real Linux cache control on disposable files, not user images."""
import hashlib
import mmap
import os
from pathlib import Path
import tempfile
import unittest

from io_host_cache import evict_images, residency


class HostCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def image(self, name="data.img"):
        path = self.root / name
        data = bytes(range(256)) * (mmap.PAGESIZE // 128)
        path.write_bytes(data)
        return path, data

    def test_evict_preserves_bytes_and_reports_real_residency(self):
        path, data = self.image()
        with path.open("rb") as stream:
            self.assertEqual(stream.read(), data)
            self.assertGreater(residency(stream.fileno())["resident_pages"], 0)
        result = evict_images(self.root, (path.name,))
        item = result["images"][path.name]
        self.assertEqual(item["after"]["resident_pages"], 0)
        self.assertEqual(hashlib.sha256(path.read_bytes()).digest(),
                         hashlib.sha256(data).digest())

    def test_reject_aliases_and_paths_outside_private_directory(self):
        path, data = self.image()
        (self.root / "alias.img").symlink_to(path)
        with self.assertRaises(OSError):
            evict_images(self.root, ("alias.img",))
        os.link(path, self.root / "hardlink.img")
        with self.assertRaises(ValueError):
            evict_images(self.root, (path.name,))
        with self.assertRaises(ValueError):
            evict_images(self.root, ("../data.img",))
        self.assertEqual(path.read_bytes(), data)

    def test_reject_partial_pages_and_empty_images(self):
        for data in (b"", b"partial"):
            path = self.root / "data.img"
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                evict_images(self.root, (path.name,))
            self.assertEqual(path.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()

"""Run with NAR_UNPACK_BIN pointing to a freshly built extractor."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


def string(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack("<Q", len(value)) + value + bytes((-len(value)) % 8)


def node(kind, *fields):
    return b"".join(map(string, ["(", "type", kind, *fields, ")"]))


def directory(entries):
    return (b"".join(map(string, ["(", "type", "directory"])) +
            b"".join(string("entry") + string("(") + string("name") +
                     string(name) + string("node") + value + string(")")
                     for name, value in entries) + string(")"))


class ArchiveTests(unittest.TestCase):
    binary = os.environ.get("NAR_UNPACK_BIN", "build/package-tools/nar-unpack")

    def extract(self, archive, target):
        return subprocess.run([self.binary, str(target)],
                              input=string("nix-archive-1") + archive,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=30)

    def test_relative_and_absolute_store_links(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "package"
            archive = directory([
                ("config", directory([("fonts", node("symlink", "target", "../share/fonts"))])),
                ("cross-package", node("symlink", "target", "/nix/store/example/lib")),
                ("share", directory([("fonts", node("regular", "contents", b"font-data"))]))])
            result = self.extract(archive, root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((root / "config/fonts").read_bytes(), b"font-data")
            self.assertEqual(os.readlink(root / "cross-package"), "/nix/store/example/lib")

    def test_extraction_cannot_follow_archive_symlink(self):
        with tempfile.TemporaryDirectory() as temp:
            outside = Path(temp) / "outside"
            outside.mkdir()
            archive = directory([
                ("redirect", node("symlink", "target", str(outside))),
                ("redirect", directory([("escaped", node("regular", "contents", b"bad"))]))])
            result = self.extract(archive, Path(temp) / "package")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((outside / "escaped").exists())

    def test_file_larger_than_string_limit_streams(self):
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "large-file"
            data = bytes(range(256)) * (20 * 1024 * 1024 // 256)
            result = self.extract(node("regular", "contents", data), target)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(target.read_bytes(), data)

    def test_entry_name_cannot_escape(self):
        with tempfile.TemporaryDirectory() as temp:
            result = self.extract(directory([("../escaped", node("regular", "contents", b"bad"))]),
                                  Path(temp) / "package")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((Path(temp) / "escaped").exists())


if __name__ == "__main__":
    unittest.main()

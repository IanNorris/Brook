#!/usr/bin/env python3
"""Host regressions on a scratch tree and a disposable offline FAT image."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import migrate_usb_maintenance as tool

REVIEWED_FILES = None


class MigrationTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        work = Path(scratch.name)
        self.mount = work / "mount"
        self.state = work / "state"
        self.mount.mkdir()
        self.state.mkdir(mode=0o700)
        (self.mount / "EFI" / "BOOT").mkdir(parents=True)
        (self.mount / "EFI" / "BOOT" / "BOOTX64.EFI").write_bytes(b"MZold")
        (self.mount / "INITRD.IMG").write_bytes(b"recovery")
        (self.mount / "BROOK.CFG").write_bytes(b"# old boot configuration\n")
        (self.mount / ".dotfile").write_bytes(b"keep this")
        (self.mount / "Recovery files").mkdir()
        (self.mount / "Recovery files" / "a.bin").write_bytes(b"archive")
        self.original = tool.inventory(self.mount)
        self.payload = dict(zip(tool.NAMES, (b"new kernel", b"new ramdisk", tool.CONFIG, b"new efi"), strict=True))

    def check_restored(self):
        tool.restore(self.mount, self.state, self.original, self.payload)
        self.assertEqual(tool.inventory(self.mount), self.original)

    def test_apply_restore(self):
        tool.migrate(self.mount, self.state, self.original, self.payload)
        self.assertEqual(tool.inventory(self.mount / tool.ARCHIVE), self.original)
        self.assertEqual((self.mount / "EFI/BOOT/BOOTX64.EFI").read_bytes(), b"new efi")
        self.assertEqual((self.mount / "BROOK.CFG").read_bytes(), tool.CONFIG)
        self.check_restored()

    def test_partial_stage_can_restore(self):
        write = tool.write_file

        def broken(path, payload):
            if path.name == "INITRD.IMG":
                write(path, payload[:6])
                raise OSError("interrupted staged file write")
            write(path, payload)

        with patch.object(tool, "write_file", side_effect=broken):
            with self.assertRaisesRegex(OSError, "interrupted"):
                tool.migrate(self.mount, self.state, self.original, self.payload)
        self.check_restored()

    def test_partial_archive_can_restore(self):
        rename = Path.rename

        def broken(path, destination):
            if path.name == ".dotfile":
                raise OSError("interrupted old tree archive")
            return rename(path, destination)

        with patch.object(Path, "rename", broken):
            with self.assertRaisesRegex(OSError, "interrupted"):
                tool.migrate(self.mount, self.state, self.original, self.payload)
        self.assertTrue((self.mount / "OLD/EFI/BOOT/BOOTX64.EFI").exists())
        self.check_restored()

    def test_partial_publish_and_partial_restore(self):
        rename = Path.rename

        def broken(path, destination):
            if path.parent.name == tool.STAGE and path.name == "EFI":
                raise OSError("interrupted before publishing EFI")
            return rename(path, destination)

        with patch.object(Path, "rename", broken):
            with self.assertRaisesRegex(OSError, "interrupted"):
                tool.migrate(self.mount, self.state, self.original, self.payload)
        self.assertFalse((self.mount / "EFI").exists())

        def broken_restore(path, destination):
            if path.parent.name == tool.ARCHIVE and path.name == "INITRD.IMG":
                raise OSError("interrupted during restoring original tree")
            return rename(path, destination)

        with patch.object(Path, "rename", broken_restore):
            with self.assertRaisesRegex(OSError, "interrupted"):
                tool.restore(self.mount, self.state, self.original, self.payload)
        self.check_restored()

    def test_modified_old_tree_is_never_erased(self):
        tool.migrate(self.mount, self.state, self.original, self.payload)
        (self.mount / "OLD/.dotfile").write_bytes(b"unexpected change")
        before = tool.inventory(self.mount)
        with self.assertRaisesRegex(ValueError, "archived original"):
            tool.restore(self.mount, self.state, self.original, self.payload)
        self.assertEqual(tool.inventory(self.mount), before)

    def test_capacity_and_reserved_names(self):
        with patch.object(tool.os, "statvfs", return_value=SimpleNamespace(f_frsize=4096, f_bavail=3)):
            with self.assertRaisesRegex(ValueError, "FAT has room"):
                tool.ensure_capacity(self.mount, self.original, self.payload)
        (self.mount / "old").mkdir()
        with self.assertRaisesRegex(ValueError, "OLD or BRKNEW"):
            tool.reject_reserved(tool.inventory(self.mount))

    def test_full_backup_readback_tamper(self):
        stick = self.state / "stick.img"
        tool.write_file(stick, b"first partition and remaining raw device bytes")
        initial = tool.digest_file(stick, private=True)
        identity = {"sysfs": "/no/fixture/disk", "fat_device": 0}
        tool.store_record(self.state / "backup.json",
                          {"image": initial, "source": identity, "original": self.original})
        before = tool.inventory(self.mount)
        with patch.object(tool, "usb_mount", return_value=identity):
            self.assertEqual(tool.receipt(self.mount, Path("/dev/test"), self.state)["image"], initial)
            stick.write_bytes(b"deliberately altered full-disk fixture")
            with self.assertRaisesRegex(ValueError, "Full private backup"):
                tool.receipt(self.mount, Path("/dev/test"), self.state)
        self.assertEqual(tool.inventory(self.mount), before)

    def test_preflight_only_allows_vfat_dirty_bit_after_ro_capture(self):
        image = self.state / "stick.img"
        primary = b"before-partition" + bytes(512) + b"other bytes"
        dirty = len(b"before-partition") + 0x41
        tool.write_file(image, primary)
        original = tool.digest_file(image, private=True)
        record = {"image": original, "source": {"fat_dirty_offset": dirty}}
        self.assertTrue(tool.matches_captured_bytes(original, record, image))
        with image.open("rb") as stream:
            expected = tool.digest_stream(stream, set_dirty_bit_at=dirty)
        transformed = bytearray(primary)
        transformed[dirty] = 1
        self.assertEqual(expected["sha256"], hashlib.sha256(transformed).hexdigest())
        self.assertTrue(tool.matches_captured_bytes(expected, record, image))
        for altered, value in ((dirty, 2), (dirty - 1, 1), (len(primary) - 1, 0)):
            changed = bytearray(transformed)
            changed[altered] = value
            actual = {"bytes": len(changed), "sha256": hashlib.sha256(changed).hexdigest()}
            self.assertFalse(tool.matches_captured_bytes(actual, record, image))
        self.assertEqual(image.read_bytes(), primary)

    def test_built_file_set(self):
        self.check_offline_fat(REVIEWED_FILES)

    def check_offline_fat(self, files):
        payload = tool.file_set(files)
        img = self.state / "fixture.fat"
        with img.open("wb") as stream:
            stream.truncate(96 << 20)

        def m(command, *arguments, **kwargs):
            return subprocess.run([command, "-i", img, *arguments], check=True, **kwargs).stdout

        m("mformat", "-F", "::")
        self.addCleanup(img.unlink, missing_ok=True)
        for name in ("EFI", "EFI/BOOT", "Recovery files"):
            m("mmd", "::/" + name)
        old_files = ("EFI/BOOT/BOOTX64.EFI", "INITRD.IMG", "BROOK.CFG", ".dotfile", "Recovery files/a.bin")
        for name in old_files:
            m("mcopy", "-", "::/" + name, input=(self.mount / name).read_bytes())
        original = {name: m("mtype", "::/" + name, capture_output=True) for name in
                    old_files}
        with img.open("rb") as disk:
            self.assertEqual(tool.vfat_dirty_byte(disk.read(512)), 0x41)
        backup = self.state / "full-backup.img"
        before = tool.digest_file(img)
        with img.open("rb") as source, backup.open("wb") as output:
            self.assertEqual(tool.digest_stream(source, output), before)
        self.assertEqual(tool.digest_file(backup), before)
        m("mmd", "::/OLD")
        for name in tool.entries(self.original):
            m("mmove", "::/" + name, "::/OLD/" + name)
        for name in original:
            self.assertEqual(m("mtype", "::/OLD/" + name, capture_output=True), original[name])
        for name in ("EFI", "EFI/BOOT", "KERNEL"):
            m("mmd", "::/" + name)
        for name, content in payload.items():
            m("mcopy", "-", "::/" + name, input=content)
            self.assertEqual(hashlib.sha256(m("mtype", "::/" + name, capture_output=True)).digest(),
                             hashlib.sha256(content).digest())
        subprocess.run(["fsck.fat", "-n", img], check=True, capture_output=True)
        m("mdeltree", "::/EFI", "::/KERNEL")
        m("mdel", "::/INITRD.IMG", "::/BROOK.CFG")
        for name in tool.entries(self.original):
            m("mmove", "::/OLD/" + name, "::/" + name)
        m("mdeltree", "::/OLD")
        for name, content in original.items():
            self.assertEqual(m("mtype", "::/" + name, capture_output=True), content)
        subprocess.run(["fsck.fat", "-n", img], check=True, capture_output=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--files", type=Path)
    options, args = parser.parse_known_args()
    REVIEWED_FILES = options.files
    if REVIEWED_FILES is None:
        MigrationTest.test_built_file_set = unittest.skip("requires --files")(MigrationTest.test_built_file_set)
    unittest.main(argv=[__file__, *args])

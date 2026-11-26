#!/usr/bin/env python3
"""Exercise first launch without opening Steam or reading any real user state."""
import os
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools/steam-bootstrap"))
sys.path.insert(0, str(PROJECT / "scripts/workloads"))
import brook_steam_seed as seed


class SteamSeedTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)
        self.source = self.home / "seed"
        self.install = self.home / "Steam"
        self.client = self.install / "client"
        binary = self.source / "steamrt64/steam"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"seed executable")
        binary.chmod(0o555)
        (self.source / "public").write_bytes(b"immutable resource")
        (self.source / "outer").symlink_to("steamrt64")

    def test_first_launch_and_new_seed_preserve_data(self):
        other = self.home / "steam-20261022"
        other.mkdir()
        (other / "legacy").write_text("previous user's installation")
        self.assertEqual(seed.ensure_client(self.source, self.install, "1"), "1")
        executable = self.client / "steamrt64/steam"
        self.assertEqual(stat.S_IMODE(executable.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((self.client / "public").stat().st_mode), 0o600)
        self.assertEqual(os.readlink(self.client / "outer"), "steamrt64")
        game = self.client / "steamapps/common/game"
        game.parent.mkdir(parents=True)
        game.write_bytes(b"user-installed files")
        login = self.client / "config"
        login.write_bytes(b"user login fixture")
        (self.source / "public").write_bytes(b"version two")
        self.assertEqual(seed.ensure_client(self.source, self.install, "2"), "1")
        self.assertEqual((self.client / "public").read_bytes(), b"immutable resource")
        self.assertEqual(game.read_bytes(), b"user-installed files")
        self.assertEqual(login.read_bytes(), b"user login fixture")
        self.assertEqual((other / "legacy").read_text(), "previous user's installation")

    def test_copy_failure_leaves_no_partial_client(self):
        with patch.object(seed.shutil, "copyfile", side_effect=OSError("write failed")):
            with self.assertRaisesRegex(OSError, "write failed"):
                seed.ensure_client(self.source, self.install, "1")
        self.assertEqual(list(self.install.iterdir()), [])
        self.assertEqual(seed.ensure_client(self.source, self.install, "1"), "1")

    def test_existing_unmanaged_tree_is_not_replaced(self):
        self.install.mkdir(mode=0o700)
        self.client.mkdir()
        private = self.client / "keep"
        private.write_bytes(b"old client data")
        with self.assertRaisesRegex(ValueError, "no Brook package receipt"):
            seed.ensure_client(self.source, self.install, "1")
        self.assertEqual(private.read_bytes(), b"old client data")

    def test_runtime_workspace_capacity(self):
        with patch.object(seed.os, "statvfs",
                          return_value=SimpleNamespace(f_bavail=1, f_frsize=4096)):
            with self.assertRaisesRegex(ValueError, "runtime workspace"):
                seed.ensure_client(self.source, self.install, "1")
        self.assertFalse(self.client.exists())


if __name__ == "__main__":
    unittest.main()

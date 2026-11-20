#!/usr/bin/env python3
import base64
import hashlib
from pathlib import Path
import tempfile
import sys
import unittest
from zipfile import ZIP_DEFLATED, ZipFile

from stage_nar import PackedCache

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from profile_cache_bundle import validate_push_capacity
from stage_profile import cache_capabilities


class PackedCacheTest(unittest.TestCase):
    def test_archive_must_match_signed_size_and_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            name = "0" * 32 + "-fixture"
            expected = b"signed raw NAR"
            sha = base64.b64encode(hashlib.sha256(expected).digest()).decode("ascii")
            entry = {"narHash": "sha256-" + sha, "narSize": len(expected), "references": []}
            meta = root / "meta.zip"
            with ZipFile(meta, "x") as bundle:
                bundle.writestr(name[:32] + ".narinfo",
                                f"StorePath: /nix/store/{name}\nNarHash: {entry['narHash']}\n"
                                f"NarSize: {len(expected)}\nCompression: none\nURL: nar/archive\n")
            path = root / "archive.zip"
            for index, raw in enumerate((expected, expected + b"!", b"x" * len(expected))):
                with ZipFile(path, "w") as bundle:
                    bundle.writestr("nar/archive", raw)
                out = root / f"raw-{index}"
                with PackedCache([meta, path]) as cache:
                    self.assertTrue(cache.contains(name))
                    if index:
                        with self.assertRaisesRegex(ValueError, "Downloaded NAR"):
                            cache.archive(name, out, entry, {})
                    else:
                        cache.archive(name, out, entry, {})
                        self.assertEqual(out.read_bytes(), expected)
            with PackedCache([meta]) as cache:
                with self.assertRaisesRegex(ValueError, "Missing packed NAR"):
                    cache.archive(name, root / "absent", entry, {})

    def test_capacity_budget_covers_all_files_before_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data").mkdir()
            live = cache_capabilities(root / "nix")
            self.assertEqual(live["bridge_cache_api"], 1)
            self.assertGreater(live["block_bytes"], 0)
            self.assertGreaterEqual(live["free_bytes"], 0)
            paths = [root / name for name in ("plan", "signature", "first", "second")]
            for file, size in zip(paths, (99, 64, 4096, 4097)):
                file.write_bytes(b"x" * size)
            capabilities = {"bridge_cache_api": 1, "block_bytes": 4096, "free_bytes": 20480}
            self.assertEqual(validate_push_capacity(paths, capabilities), 20480)
            capabilities["free_bytes"] -= 1
            with self.assertRaisesRegex(ValueError, "needs 20480 guest bytes"):
                validate_push_capacity(paths, capabilities)
            capabilities["bridge_cache_api"] = 0
            with self.assertRaisesRegex(ValueError, "Invalid bridge cache capacity"):
                validate_push_capacity(paths, capabilities)

    def test_invalid_names_compression_and_duplicate_members(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first, second = root / "first.zip", root / "second.zip"
            for invalid in ("../existing", "nar/..", "/nar/archive", "nested/nar/archive"):
                with ZipFile(first, "w") as bundle:
                    bundle.writestr(invalid, "payload")
                with self.assertRaisesRegex(ValueError, "Unexpected cache bundle member"):
                    with PackedCache([first]):
                        self.fail("unsafe member")
            with ZipFile(first, "w", compression=ZIP_DEFLATED) as bundle:
                bundle.writestr("nar/archive", "compressed again")
            with self.assertRaisesRegex(ValueError, "Unexpected cache bundle member"):
                with PackedCache([first]):
                    self.fail("compressed member")
            for path in (first, second):
                with ZipFile(path, "w") as bundle:
                    bundle.writestr("nar/archive", "payload")
            with self.assertRaisesRegex(ValueError, "Unexpected cache bundle member"):
                with PackedCache([first, second]):
                    self.fail("duplicate across two transfers")


if __name__ == "__main__":
    unittest.main()

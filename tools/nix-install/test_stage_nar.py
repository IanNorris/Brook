#!/usr/bin/env python3
"""Exercise the signed NAR import path with actual compressed Nix archives."""

import base64
import functools
import hashlib
import http.server
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

import stage_nar

ROOT = Path(__file__).resolve().parents[2]


class NarImport(unittest.TestCase):
    @unittest.skipUnless(all(shutil.which(name) for name in
                             ("cc", "curl", "nix-store", "xz", "zstd")),
                         "requires the host Nix and archive tools")
    def test_compressed_upstream_import_and_installed_path_conflict(self):
        with tempfile.TemporaryDirectory(prefix="brook-stage-nar-") as scratch:
            folder = Path(scratch)
            tree = folder / "tree"
            tree.mkdir()
            payload = tree / "program"
            payload.write_bytes(bytes(range(256)) * 12289)
            payload.chmod(0o755)
            (tree / "link").symlink_to("program")
            archive = subprocess.check_output(["nix-store", "--dump", tree])
            self.assertGreater(len(archive), 2 * stage_nar.CHUNK)
            digest = base64.b64encode(hashlib.sha256(archive).digest()).decode("ascii")
            entry = {"narHash": "sha256-" + digest, "narSize": len(archive),
                     "references": [], "source": "upstream"}
            unpack = folder / "nar-unpack"
            subprocess.run(["cc", ROOT / "tools/nar-unpack/nar-unpack.c", "-o", unpack], check=True)
            tools = {name: shutil.which(name) for name in ("curl", "xz", "zstd")}
            tools["nar-unpack"] = str(unpack)
            store = folder / "store"
            store.mkdir()
            served = folder / "cache"
            served.mkdir()
            handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=served)
            with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
                address = f"http://127.0.0.1:{server.server_port}"
                cache = {"brook": "http://127.0.0.1:1", "mirrorUpstream": False}
                worker = threading.Thread(target=server.serve_forever)
                worker.start()
                try:
                    with patch.object(stage_nar, "UPSTREAM", address):
                        for index, compression in enumerate(("xz", "zstd")):
                            with self.subTest(compression=compression):
                                path_hash = str(index) * 32
                                name = path_hash + "-test"
                                archive_name = "archive.nar." + compression
                                narinfo = served / (path_hash + ".narinfo")
                                # Real upstream cache omits References for empty lists.
                                narinfo.write_text(
                                    f"StorePath: /nix/store/{name}\nURL: {archive_name}\n"
                                    f"Compression: {compression}\nNarSize: {len(archive)}\n"
                                    f"NarHash: {entry['narHash']}\n")
                                compressed = served / archive_name
                                compressed.write_bytes(subprocess.check_output(
                                    [tools[compression], "-c"], input=archive))
                                self.assertEqual(stage_nar.ensure_path(
                                    store, name, entry, cache, tools), "upstream")
                                target = store / name
                                self.assertEqual((target / "program").read_bytes(), payload.read_bytes())
                                self.assertTrue((target / "program").stat().st_mode & 0o100)
                                self.assertEqual((target / "link").readlink(), Path("program"))
                                self.assertEqual(stage_nar.ensure_path(
                                    store, name, entry, cache, tools), "present")

                                extra = target / "old-wrapper"
                                extra.write_text("locally staged file")
                                with patch.object(stage_nar, "read_narinfo",
                                                  side_effect=AssertionError("fetch on conflict")):
                                    with self.assertRaisesRegex(ValueError,
                                                                "Existing store path failed verification"):
                                        stage_nar.ensure_path(store, name, entry, cache, tools)
                                self.assertTrue(extra.is_file())

                                shutil.rmtree(target)
                                compressed.write_bytes(subprocess.check_output(
                                    [tools[compression], "-c"], input=archive + b"x"))
                                with self.assertRaisesRegex(ValueError, "Downloaded NAR exceeds"):
                                    stage_nar.ensure_path(store, name, entry, cache, tools)
                                self.assertFalse(target.exists())
                                wrong = bytearray(archive)
                                wrong[archive.index(payload.read_bytes()[:256])] ^= 1
                                compressed.write_bytes(subprocess.check_output(
                                    [tools[compression], "-c"], input=wrong))
                                with self.assertRaisesRegex(ValueError, "does not match the signed plan"):
                                    stage_nar.ensure_path(store, name, entry, cache, tools)
                                self.assertFalse(target.exists())
                                self.assertEqual(list(store.glob(".brook-nar-*")), [])
                finally:
                    server.shutdown()
                    worker.join()


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
import functools
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from stage_closure import generate_plan, publish_cache
from profile_cache_bundle import pack_cache
import stage_nar
from stage_profile import PROFILE_ALIAS, apply, initialize, rollback, validate_plan, verified_plan

NIX = ["nix", "--extra-experimental-features", "nix-command"]


class PlanIntegration(unittest.TestCase):
    @unittest.skipUnless(all(shutil.which(tool) for tool in ("nix", "openssl", "curl", "zstd", "cc")),
                         "requires the host cache tools and OpenSSL")
    def test_signed_cache_and_profile_generation(self):
        with tempfile.TemporaryDirectory(prefix="brook-closure-fixture-") as scratch:
            folder = Path(scratch)
            root = folder / "device/nix"
            for name in ("profile/bin", "var", "store", "share/applications/icons", "etc/brook"):
                (root / name).mkdir(parents=True, exist_ok=True)
            (root / "share/applications/applications.idx").write_text("legacy\n")
            (root / "profile/bin/old").write_text("legacy")
            user_package = "0" * 32 + "-user-installed"
            (root / "store" / user_package / "bin").mkdir(parents=True)
            (root / "store" / user_package / "bin/user-app").write_text("user program")
            (root / "profile/bin/user-app").symlink_to(f"../../store/{user_package}/bin/user-app")
            (root / "profile/manifest.tsv").write_text(f"User App\t1.0\t{user_package}\n")
            initialize(root)
            initialize(root)

            profile = folder / "profile"
            (profile / "bin").mkdir(parents=True)
            (profile / "bin/session.conf").write_text("session settings\n")
            (profile / "share/applications").mkdir(parents=True)
            (profile / "share/applications/Test.desktop").write_text(
                "[Desktop Entry]\nType=Application\nName=Fixture\n"
                "Exec=/nix/profile/bin/session.conf\n")
            built = Path(subprocess.check_output(
                ["nix-store", "--add", profile], text=True).strip())
            extra = folder / "dependency"
            extra.write_text("upstream archive\n")
            dependency = Path(subprocess.check_output(
                ["nix-store", "--add", extra], text=True).strip())
            existing = folder / "existing"
            existing.write_text("already present\n")
            installed = Path(subprocess.check_output(
                ["nix-store", "--add", existing], text=True).strip())
            shutil.copyfile(installed, root / "store" / installed.name)

            secret = folder / "cache.key"
            with os.fdopen(os.open(secret, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as stream:
                subprocess.run([*NIX, "key", "generate-secret", "--key-name", "brook-stage-test"],
                               check=True, stdout=stream)
            unpack = folder / "nar-unpack"
            subprocess.run(["cc", ROOT / "tools/nar-unpack/nar-unpack.c", "-o", unpack], check=True)
            key = folder / "plan.pem"
            pub = root / "etc/brook/stage-plan.pub"
            subprocess.run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", key], check=True)
            subprocess.run(["openssl", "pkey", "-in", key, "-pubout", "-out", pub], check=True)

            cache = folder / "cache"
            handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=cache)
            with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
                address = f"http://127.0.0.1:{server.server_port}"
                with patch("stage_closure.upstream_has", lambda name, _: name == dependency.name):
                    plan = generate_plan(built, (dependency, installed), address, mirror_upstream=True)
                publish_cache(plan["closure"], cache, secret, mirror_upstream=True)
                worker = threading.Thread(target=server.serve_forever, daemon=True)
                worker.start()
                self.addCleanup(worker.join)
                try:
                    document = folder / "plan.json"
                    document.write_text(json.dumps(plan))
                    sig = folder / "plan.sig"
                    subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", key,
                                    "-in", document, "-out", sig], check=True)
                    signed_plan, digest = verified_plan(document, sig, pub, shutil.which("openssl"))
                    document.write_text(json.dumps(plan) + " ")
                    with self.assertRaises(subprocess.CalledProcessError):
                        verified_plan(document, sig, pub, shutil.which("openssl"))
                    tools = {name: shutil.which(name) for name in ("curl", "xz", "zstd")}
                    tools["nar-unpack"] = str(unpack)
                    tx = "1" * 32
                    # No real network dependency in this fixture. Force one upstream
                    # failure and verify a machine with host-only reachability can apply.
                    with patch.object(stage_nar, "UPSTREAM", "http://127.0.0.1:1"):
                        apply(root, signed_plan, digest, tx, tools)
                    self.assertEqual(os.readlink(root / PROFILE_ALIAS), ".brook-profile-" + tx)
                    self.assertEqual((root / "profile/bin/session.conf").read_text(), "session settings\n")
                    self.assertTrue((root / "profile/bin/user-app").is_file())
                    self.assertIn("User App", (root / "profile/manifest.tsv").read_text())
                    self.assertIn("Fixture", (root / "share/applications/applications.idx").read_text())
                    self.assertEqual((root / "store" / dependency.name).read_text(), extra.read_text())
                    (root / "store" / dependency.name).unlink()
                    with patch.object(stage_nar, "UPSTREAM", address):
                        self.assertEqual(stage_nar.ensure_path(
                            root / "store", dependency.name, plan["closure"][dependency.name],
                            plan["cache"], tools), "upstream")
                    apply(root, signed_plan, digest, tx, tools)
                    rollback(root, tx)
                    self.assertEqual((root / "share/applications/applications.idx").read_text(), "legacy\n")
                    self.assertTrue((root / "profile/bin/old").is_file())

                    # A separate signed push-only plan must work without curl,
                    # including for mirrored upstream NARs.
                    plan["cache"]["brook"] = None
                    document.write_text(json.dumps(plan))
                    subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", key,
                                    "-in", document, "-out", sig], check=True)
                    offline_plan, offline_digest = verified_plan(document, sig, pub, shutil.which("openssl"))
                    bundle_dir = folder / "bundles"
                    bundle_dir.mkdir()
                    bundles = list(pack_cache(cache, offline_plan, bundle_dir, limit=8192))
                    shutil.rmtree(root / "store" / built.name)
                    (root / "store" / dependency.name).unlink()
                    with self.assertRaisesRegex(ValueError, "requires a bridge-pushed cache"):
                        apply(root, offline_plan, offline_digest, "3" * 32, tools)
                    with patch.object(stage_nar, "curl", side_effect=AssertionError("unexpected network")):
                        with stage_nar.PackedCache(bundles) as bundled:
                            apply(root, offline_plan, offline_digest, "3" * 32, tools, packed_cache=bundled)
                    self.assertEqual((root / "profile/bin/session.conf").read_text(), "session settings\n")
                    self.assertEqual((root / "store" / dependency.name).read_bytes(), extra.read_bytes())
                    rollback(root, "3" * 32)

                    # Keep old store data in place and refuse a new switch when
                    # the bytes at any closure path no longer match the signed plan.
                    (root / "store" / installed.name).write_text("modified data\n")
                    with self.assertRaises(ValueError):
                        apply(root, signed_plan, digest, "2" * 32, tools)
                    self.assertEqual(os.readlink(root / PROFILE_ALIAS), ".brook-profile-legacy")
                    plan["profile"]["root"] = "../bad"
                    with self.assertRaises(ValueError):
                        validate_plan(plan)
                finally:
                    server.shutdown()


if __name__ == "__main__":
    unittest.main()

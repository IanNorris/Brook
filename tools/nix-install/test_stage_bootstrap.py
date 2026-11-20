#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from dell_bootstrap_stage_profile import guest_program, manifest_for, runtime_archive, sha


class BootstrapTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("nix") and shutil.which("cc"), "needs Nix and C compiler")
    def test_legacy_bootstrap_round_trip(self):
        with tempfile.TemporaryDirectory(prefix="brook-bootstrap-test-") as directory:
            work = Path(directory)
            root = work / "target"
            nix = root / "nix"
            for name in ("profile/bin", "store", "share/applications/icons", "etc", "var", "bin"):
                (nix / name).mkdir(parents=True, exist_ok=True)
            for name in ("brook-run", "brook-chromium", "brook-firefox", "brook-webapp", "brook-apps-index"):
                (nix / "profile/bin" / name).write_text(name)
            (nix / "share/applications/applications.idx").write_text("existing hand-staged index\n")
            install = nix / "bin/nix-install"
            install.write_bytes(b"original installer")
            install.chmod(0o755)
            (nix / "bin/nix").symlink_to("nix-install")
            subprocess.run(["cc", ROOT / "tools/nar-unpack/nar-unpack.c",
                            "-o", nix / "bin/nar-unpack"], check=True)

            fixture = work / "helper"
            (fixture / "bin").mkdir(parents=True)
            helper = fixture / "bin/brook-stage-profile"
            helper.write_text(f"#!{sys.executable}\nimport sys\n"
                              f"sys.path.insert(0, {str(ROOT / 'scripts')!r})\n"
                              f"sys.path.insert(0, {str(ROOT / 'tools/nix-install')!r})\n"
                              "from stage_profile import main\nmain()\n")
            helper.chmod(0o755)
            path = Path(subprocess.check_output(["nix-store", "--add", fixture], text=True).strip())
            stage_py = work / "bootstrap.bin"
            guest_program(stage_py)
            key, updated = work / "stage-plan.pub", work / "nix-install"
            key.write_bytes(b"fixture key")
            updated.write_bytes(b"candidate installer")
            manifest = work / "manifest.json"
            manifest.write_text(json.dumps(manifest_for(path, updated, key), sort_keys=True))
            identity = sha(manifest)

            def command(action, *args, fail=False, digest=identity):
                result = subprocess.run(
                    [sys.executable, stage_py, action, digest, *args],
                    env=dict(os.environ, BROOK_STAGE_ROOT=str(root)), capture_output=True, text=True)
                if fail:
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    return result.stderr
                self.assertEqual(result.returncode, 0, result.stderr)
                records = [line.removeprefix("STAGE_BOOTSTRAP ") for line in result.stdout.splitlines()
                           if line.startswith("STAGE_BOOTSTRAP ")]
                return json.loads(records[-1])

            initial = command("check", manifest)
            self.assertEqual(initial["missing"], [0])
            self.assertFalse((nix / "profile").is_symlink())
            self.assertFalse((nix / "etc/brook").exists())

            archive = work / "runtime.tar"
            runtime_archive(archive, json.loads(manifest.read_text()), initial["missing"], updated, key)
            install.write_bytes(b"unrelated change after check")
            self.assertIn("differs from the bootstrap preflight",
                          command("apply", manifest, archive, json.dumps(initial), fail=True))
            install.write_bytes(b"original installer")

            # No payload copies are needed when the exact store path exists.
            shutil.copytree(path, nix / "store" / path.name)
            reusable = command("check", manifest)
            self.assertEqual(reusable["missing"], [])
            state = command("apply", manifest, archive, json.dumps(reusable))
            self.assertEqual(state["phase"], "complete")
            self.assertEqual(state["generation"], ".brook-profile-legacy")
            self.assertEqual(install.read_bytes(), updated.read_bytes())
            self.assertEqual((nix / "etc/brook/stage-plan.pub").read_bytes(), key.read_bytes())
            self.assertEqual((nix / "etc/brook/stage-plan.pub").stat().st_mode & 0o777, 0o644)
            self.assertEqual(command("apply", manifest, archive, json.dumps(state)), state)
            self.assertTrue((nix / "profile/bin/brook-webapp").is_file())

            # Refresh the helper with a new closure while keeping the active
            # profile, install binary and original bootstrap receipt.
            old_link = (nix / "bin/brook-stage-profile").readlink()
            helper.write_text(helper.read_text() + "# replacement helper\n")
            replacement = Path(subprocess.check_output(
                ["nix-store", "--add", fixture], text=True).strip())
            refresh_manifest = work / "refresh.json"
            refresh = manifest_for(replacement, updated, key)
            refresh_manifest.write_text(json.dumps(refresh, sort_keys=True))
            refresh_id = sha(refresh_manifest)
            refresh_check = command("refresh-check", refresh_manifest, digest=refresh_id)
            self.assertEqual(refresh_check["phase"], "refresh-required")
            self.assertEqual((nix / "bin/brook-stage-profile").readlink(), old_link)
            self.assertEqual(refresh_check["missing"], [0])
            refresh_archive = work / "refresh.tar"
            runtime_archive(refresh_archive, refresh, refresh_check["missing"], updated, key)
            state = command("refresh", refresh_manifest, refresh_archive,
                            json.dumps(refresh_check), digest=refresh_id)
            self.assertEqual(state["phase"], "complete")
            self.assertEqual(state["generation"], ".brook-profile-legacy")
            self.assertEqual(install.read_bytes(), updated.read_bytes())
            self.assertEqual((nix / "bin/brook-stage-profile").readlink(),
                             replacement / "bin/brook-stage-profile")
            self.assertEqual(command("refresh", refresh_manifest, refresh_archive,
                                     json.dumps(refresh_check), digest=refresh_id), state)

            self.assertEqual(command("rollback")["phase"], "rolled_back")
            self.assertEqual(install.read_bytes(), b"original installer")
            self.assertEqual((nix / "share/applications/applications.idx").read_text(),
                             "existing hand-staged index\n")
            self.assertFalse((nix / "profile").is_symlink())
            self.assertFalse((nix / "etc/brook/stage-plan.pub").exists())
            self.assertEqual(command("rollback")["phase"], "rolled_back")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Check managed generation fidelity with the Dell's public app rows."""

import base64
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from stage_launchers import desktop_command
from stage_profile import apply, initialize, validate_plan

FIXTURE = Path(__file__).parent / "fixtures/legacy-launcher.idx"
STORE_PATH = "0" * 32 + "-test-profile"
GALLERY_PATH = "1" * 32 + "-test-gallery"
SIGNED_APPS = {"brook-gallery", "brook-hardware-chromium", "org.gnome.Nautilus",
               "brook-hardware-firefox", "brook-settings", "org.xfce.mousepad",
               "brook-quake2"}
Q2 = "/nix/profile/bin/brook-quake2"


def script(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n")
    path.chmod(0o755)


class LauncherFidelity(unittest.TestCase):
    @unittest.skipUnless(shutil.which("nix-store"), "requires Nix")
    def test_legacy_generation_survives_apply(self):
        with tempfile.TemporaryDirectory(prefix="brook-launcher-fixture-") as scratch:
            root = Path(scratch)
            nix = root / "nix"
            for directory in ("bin", "store", "profile/bin", "share/applications/icons", "var"):
                (nix / directory).mkdir(parents=True)
            index = (nix / "share/applications/applications.idx")
            shutil.copyfile(FIXTURE, index)
            old_rows = [row.split("\t") for row in index.read_text().splitlines()]
            signed = nix / "store" / STORE_PATH
            gallery = nix / "store" / GALLERY_PATH
            bins = {"brook-run": signed, "nemo": signed}
            directories = {"bin", "share", "share/applications"}
            links = {"bin/brook-run": f"/nix/store/{STORE_PATH}/bin/brook-run",
                     "bin/nemo": f"/nix/store/{STORE_PATH}/bin/nemo"}
            apps = signed / "share/applications"
            apps.mkdir(parents=True)
            for title, command, icon, categories, app_id in old_rows:
                words = command.removeprefix("--uid 1000 ").split()
                if Path(words[0]).name == "brook-run":
                    words.pop(0)
                target = Path(words[0])
                if target.is_absolute() and str(target).startswith("/data/"):
                    script(root / str(target)[1:])
                elif target.is_absolute() and str(target).startswith("/nix/store/"):
                    package = root / str(target)[1:]
                    script(package)
                    (nix / "profile/bin" / target.name).symlink_to(
                        "../../store/" + str(target).removeprefix("/nix/store/"))
                else:
                    package = signed if app_id in SIGNED_APPS else gallery
                    bins[target.name] = package
                    if package == gallery:
                        (nix / "profile/bin" / target.name).unlink(missing_ok=True)
                        (nix / "profile/bin" / target.name).symlink_to(
                            f"../../store/{GALLERY_PATH}/bin/{target.name}")
                    else:
                        links["bin/" + target.name] = f"/nix/store/{STORE_PATH}/bin/{target.name}"
                if icon:
                    (nix / "share/applications/icons" / icon).write_bytes(bytes(2304))
                if app_id in SIGNED_APPS:
                    desktop = app_id + ".desktop"
                    unwrapped = command.removeprefix("--uid 1000 ")
                    (apps / desktop).write_text(
                        f"[Desktop Entry]\nName={title}\nExec={unwrapped}\nCategories={categories}\n")
                    links["share/applications/" + desktop] = f"/nix/store/{STORE_PATH}/share/applications/{desktop}"
            assert bins["brook-nautilus"] == signed
            (apps / "nemo.desktop").write_text("[Desktop Entry]\nName=Files\nExec=nemo\n")
            links["share/applications/nemo.desktop"] = f"/nix/store/{STORE_PATH}/share/applications/nemo.desktop"
            for name, package in bins.items():
                script(package / "bin" / name)

            initialize(nix)
            archive = subprocess.check_output(["nix-store", "--dump", signed])
            digest = base64.b64encode(hashlib.sha256(archive).digest()).decode("ascii")
            plan = {"format": 1, "packages": [], "roots": [STORE_PATH],
                    "closure": {STORE_PATH: {"narHash": "sha256-" + digest,
                                             "narSize": len(archive), "references": [], "source": "brook"}},
                    "cache": {"brook": "http://127.0.0.1:1", "mirrorUpstream": False},
                    "profile": {"links": links, "directories": sorted(directories)}}
            validate_plan(plan)
            apply(nix, plan, "fixture", "f" * 32, {})

            result = nix / "profile/.brook-launcher/applications.idx"
            expected = []
            for row in old_rows:
                _, command, _, _, app_id = row
                if not command.startswith("--uid 1000 ") and (
                        app_id not in SIGNED_APPS or command == Q2):
                    row[1] = "--uid 1000 " + command
                expected.append("\t".join(row))
            self.assertEqual(result.read_bytes(), ("\n".join(expected) + "\n").encode())
            saved = nix / "var/.brook-profiles/.brook-profile-legacy/.brook-launcher/applications.idx"
            self.assertEqual(saved.read_bytes(), FIXTURE.read_bytes())

    def test_carried_exec_commands_cannot_add_privileged_run_options(self):
        with tempfile.TemporaryDirectory(prefix="brook-launcher-exec-") as scratch:
            root = Path(scratch)
            nix = root / "nix"
            profile = nix / "profile"
            for tool in ("app", "brook-run"):
                script(profile / "bin" / tool)
            command = "--uid 1000 /nix/profile/bin/brook-run app --safe"
            self.assertEqual(desktop_command(command, nix, profile), command)
            for command in (
                "--uid 0 /nix/profile/bin/app",
                "--uid 1000 --uid 0 /nix/profile/bin/app",
                '--uid 1000 "--uid" 0 /nix/profile/bin/app',
                "--gid 0 /nix/profile/bin/app",
                "--uid 1000 /nix/profile/bin/brook-run --bad app",
                "--uid 1000 /nix/profile/bin/brook-run missing",
                "--uid 1000 /nix/profile/bin/../bin/app",
                "--uid 1000 /nix/profile/bin/app\n--uid 0",
                "--uid 1000 /nix/profile/bin/app # ignore suffix",
            ):
                with self.subTest(command=command):
                    self.assertIsNone(desktop_command(command, nix, profile))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Exercise the selected-profile helper in a disposable managed store."""

import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from stage_nar import layout_digest, profile_layout
from stage_selected_launchers import read_index
import stage_profile as legacy

SOURCES = Path(__file__).resolve().parent
OPENSSL = shutil.which("openssl")


def executable(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n")
    path.chmod(0o755)


def desktop(path, title, command):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"[Desktop Entry]\nName={title}\nExec=/nix/profile/bin/{command}\n")


class SelectedGenerationTest(unittest.TestCase):
    def setUp(self):
        self.work = Path(self.enterContext(tempfile.TemporaryDirectory()))
        module_path = self.work / "stage_profile.py"
        shutil.copyfile(SOURCES / "stage_profile.py", module_path)
        with (SOURCES / "stage_selected_profile.patch").open("rb") as changes:
            subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-d", self.work],
                           stdin=changes, check=True, stdout=subprocess.DEVNULL)
        spec = importlib.util.spec_from_file_location("test_selected_stage_helper", module_path)
        self.helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.helper)
        self.nix = self.work / "nix"
        self.store = self.nix / "store"
        for directory in ("profile/bin", "profile/share/applications",
                          "share/applications", "etc/brook", "store", "var"):
            (self.nix / directory).mkdir(parents=True, exist_ok=True)
        self.old_rows = self.nix / "share/applications/applications.idx"
        self.gallery_name = "1" * 32 + "-gallery"
        gallery = self.store / self.gallery_name
        legacy_profile = self.nix / "profile"
        for name in ("gallery-editor", "unsafe-editor"):
            executable(gallery / "bin" / name)
            desktop(gallery / "share/applications" / (name + ".desktop"), name, name)
            (legacy_profile / "bin" / name).symlink_to(f"../../store/{self.gallery_name}/bin/{name}")
            (legacy_profile / "share/applications" / (name + ".desktop")).symlink_to(
                f"../../../store/{self.gallery_name}/share/applications/{name}.desktop")
        (legacy_profile / "manifest.tsv").write_text(f"Gallery Editor\t1\t{self.gallery_name}\n")
        self.settings = self.work / "data/home/settings"
        executable(self.settings)
        saved_game = self.settings.parent / "steam"
        executable(saved_game)
        self.old_rows.write_text(
            "Calculator\t--uid 1000 /nix/profile/bin/brook-run selected-calc --keep-state\t\tUtility;\t"
            "brook.calc\tPrevious explanation\n"
            "Old app\tcli-extra\t\tUtility;\told-desktop\n"
            "Steam\t/data/home/steam\t\tGame;\tlegacy-steam\n"
            "Gallery Editor\tgallery-editor\t\tUtility;\tgallery-editor\tGallery comment\n"
            "Unsafe Editor\t--uid 0 /nix/profile/bin/unsafe-editor\t\tUtility;\tunsafe-editor\n"
        )
        self.helper.initialize(self.nix)

    def plan(self, digit, app_id, name, command):
        root = self.store / (digit * 32 + "-selection")
        for tool in ("brook-run", "cli-extra", command):
            executable(root / "bin" / tool)
        desktop(root / "share/applications" / (app_id + ".desktop"), name, command)
        archive = subprocess.check_output(["nix-store", "--dump", root])
        digest = base64.b64encode(hashlib.sha256(archive).digest()).decode("ascii")
        closure = {root.name: {"narHash": "sha256-" + digest, "narSize": len(archive),
                               "source": "brook", "references": []}}
        return {"format": 3, "roots": [root.name], "closure": closure, "packages": [],
                "profile": {"root": root.name, "launcherIds": [app_id],
                            "layoutSha256": layout_digest(profile_layout(root, closure, store=self.store))},
                "cache": {"brook": "http://127.0.0.1:1", "mirrorUpstream": False}}

    def active_rows(self):
        return {row[4]: row for row in read_index(self.old_rows)}

    @unittest.skipUnless(OPENSSL and shutil.which("nix-store"), "requires host OpenSSL and nix-store")
    def test_signed_selection_switches_and_rolls_back(self):
        private = self.work / "plan.pem"
        public = self.nix / "etc/brook/stage-plan.pub"
        subprocess.run([OPENSSL, "genpkey", "-algorithm", "ED25519", "-out", private], check=True)
        subprocess.run([OPENSSL, "pkey", "-in", private, "-pubout", "-out", public], check=True)
        original = self.old_rows.read_bytes()
        for digit, app_id, title, command in (("2", "brook.calc", "Calculator", "selected-calc"),
                                             ("3", "brook.clock", "Clock", "selected-clock")):
            plan = self.plan(digit, app_id, title, command)
            with self.assertRaisesRegex(ValueError, "Unknown closure plan version"):
                legacy.validate_plan(plan)
            self.assertNotIn("selected_profile_api", legacy.cache_capabilities(self.nix))
            self.assertEqual(self.helper.cache_capabilities(self.nix)["selected_profile_api"], 1)
            document = self.work / "plan.json"
            document.write_text(json.dumps(plan))
            signature = self.work / "plan.sig"
            subprocess.run([OPENSSL, "pkeyutl", "-sign", "-rawin", "-inkey", private,
                            "-in", document, "-out", signature], check=True)
            checked, digest = self.helper.verified_plan(document, signature, public, OPENSSL)
            with patch.object(self.helper.os, "sync"):
                self.helper.apply(self.nix, checked, digest, digit * 32, {})
            rows = self.active_rows()
            self.assertEqual(set(rows), {app_id, "gallery-editor"})
            self.assertEqual(rows["gallery-editor"][1], "--uid 1000 gallery-editor")
            self.assertEqual(rows["gallery-editor"][5], "Gallery comment")
            self.assertIn("Gallery Editor", (self.nix / "profile/manifest.tsv").read_text())
            for name in ("gallery-editor", "unsafe-editor", "cli-extra"):
                self.assertTrue((self.nix / "profile/bin" / name).is_file())
            if app_id == "brook.calc":
                self.assertEqual(rows[app_id][1],
                                 "--uid 1000 /nix/profile/bin/brook-run selected-calc --keep-state")
                self.assertEqual(rows[app_id][5], "Previous explanation")
            else:
                self.assertEqual(rows[app_id][1], "--uid 1000 /nix/profile/bin/selected-clock")

        self.helper.rollback(self.nix, "3" * 32)
        self.assertEqual(set(self.active_rows()), {"brook.calc", "gallery-editor"})
        self.helper.rollback(self.nix, "2" * 32)
        self.assertEqual(self.old_rows.read_bytes(), original)
        self.assertEqual(self.settings.read_text(), "#!/bin/sh\n")

    @unittest.skipUnless(shutil.which("nix-store"), "requires nix-store")
    def test_gallery_version_wins_for_the_same_desktop_id(self):
        gallery = self.store / self.gallery_name
        executable(gallery / "bin/gallery-calc")
        desktop(gallery / "share/applications/brook.calc.desktop", "My Calculator", "gallery-calc")
        profile = self.nix / "profile"
        (profile / "bin/gallery-calc").symlink_to(f"../../store/{self.gallery_name}/bin/gallery-calc")
        (profile / "share/applications/brook.calc.desktop").symlink_to(
            f"../../../store/{self.gallery_name}/share/applications/brook.calc.desktop")
        self.old_rows.write_text("My Calculator\tgallery-calc\t\tUtility;\tbrook.calc\tGallery version\n")
        plan = self.plan("5", "brook.calc", "Calculator", "selected-calc")
        with patch.object(self.helper.os, "sync"):
            self.helper.apply(self.nix, plan, "fixture", "5" * 32, {})
        self.assertEqual(self.active_rows()["brook.calc"],
                         ["My Calculator", "--uid 1000 gallery-calc", "", "Utility;",
                          "brook.calc", "Gallery version"])

    @unittest.skipUnless(shutil.which("nix-store"), "requires nix-store")
    def test_recipe_mismatch_cannot_switch_generation(self):
        plan = self.plan("4", "brook.calc", "Calculator", "selected-calc")
        plan["profile"]["launcherIds"] = ["not-selected"]
        previous = os.readlink(self.nix / legacy.PROFILE_ALIAS)
        with self.assertRaisesRegex(ValueError, "differ from the signed selection"):
            self.helper.apply(self.nix, plan, "fixture", "4" * 32, {})
        self.assertEqual(os.readlink(self.nix / legacy.PROFILE_ALIAS), previous)


if __name__ == "__main__":
    unittest.main()

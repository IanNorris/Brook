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
from stage_selected_launchers import read_index, validate_launcher_ids
from stage_selected_generations import pin_generation, prune_generations
from stage_selected_space import layout_usage, tree_inodes
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
        def patched_module(name, patch_name):
            module_path = self.work / (name + ".py")
            shutil.copyfile(SOURCES / module_path.name, module_path)
            with (SOURCES / patch_name).open("rb") as changes:
                subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-d", self.work],
                               stdin=changes, check=True, stdout=subprocess.DEVNULL)
            spec = importlib.util.spec_from_file_location("test_selected_" + name, module_path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module

        self.nar = patched_module("stage_nar", "stage_selected_nar.patch")
        with patch.dict(sys.modules, {"stage_nar": self.nar}):
            self.helper = patched_module("stage_profile", "stage_selected_profile.patch")
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
                               "source": "brook", "references": [], "storeInodes": tree_inodes(root)}}
        layout = profile_layout(root, closure, store=self.store)
        return {"format": 3, "roots": [root.name], "closure": closure, "packages": [],
                "profile": {"root": root.name, "launcherIds": [app_id],
                            "layoutSha256": layout_digest(layout), "space": layout_usage(layout)},
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
    def test_gallery_basename_and_unicode_title_survive(self):
        app_id = "éditeur+tools @latest"
        title = "Gallery\u2028Editor"
        gallery = self.store / self.gallery_name
        profile = self.nix / "profile"
        desktop(gallery / "share/applications" / (app_id + ".desktop"), title, "gallery-editor")
        (profile / "share/applications" / (app_id + ".desktop")).symlink_to(
            f"../../../store/{self.gallery_name}/share/applications/{app_id}.desktop")
        self.old_rows.write_text(
            f"{title}\tgallery-editor\t\tUtility;\t{app_id}\tGallery comment\n"
            "Unowned title\tgallery-editor\t\tUtility;\tunowned+app\n"
        )
        with self.assertRaisesRegex(ValueError, "Invalid selected profile launcher IDs"):
            validate_launcher_ids([app_id])
        plan = self.plan("6", "brook.calc", "Calculator", "selected-calc")
        with patch.object(self.helper.os, "sync"):
            self.helper.apply(self.nix, plan, "fixture", "6" * 32, {})
        rows = self.active_rows()
        self.assertEqual(set(rows), {"brook.calc", app_id})
        self.assertEqual(rows[app_id][:2], [title, "--uid 1000 gallery-editor"])
        self.assertEqual(rows[app_id][5], "Gallery comment")

    @unittest.skipUnless(all(shutil.which(tool) for tool in ("nix-store", "cc")), "requires nix-store and cc")
    def test_only_uncommitted_imports_owned_by_failed_apply_are_removed(self):
        unpack = self.work / "nar-unpack"
        subprocess.run(["cc", ROOT / "tools/nar-unpack/nar-unpack.c", "-o", unpack], check=True)
        tools = {"nar-unpack": str(unpack)}

        class Cache:
            def __init__(self, archive):
                self.data = archive

            def contains(self, name):
                return True

            def archive(self, name, output, entry, tools):
                output.write_bytes(self.data)

        for digit, action in (("7", "clean"), ("8", "replaced"), ("9", "journaled")):
            with self.subTest(failure=action):
                plan = self.plan(digit, "brook.calc", "Calculator", "selected-calc")
                target = self.store / plan["profile"]["root"]
                archive = subprocess.check_output(["nix-store", "--dump", target])
                shutil.rmtree(target)
                original = self.nix / legacy.PROFILE_ALIAS
                previous = os.readlink(original)

                def fail_prepare(*args):
                    self.assertTrue(target.is_dir())
                    if action == "replaced":
                        target.rename(self.work / "replaced-original")
                        target.write_bytes(b"external replacement")
                    raise ValueError("fixture abort before publication")

                if action == "journaled":
                    failure = patch.object(self.helper, "switch", side_effect=OSError("fixture after journal"))
                    expected = OSError
                else:
                    failure = patch.object(self.helper, "prepare", side_effect=fail_prepare)
                    expected = ValueError
                with failure, patch.object(self.helper.os, "sync"), self.assertRaisesRegex(expected, "fixture"):
                    self.helper.apply(self.nix, plan, "fixture", digit * 32, tools, packed_cache=Cache(archive))
                self.assertEqual(os.readlink(original), previous)
                self.assertTrue((self.store / self.gallery_name).is_dir())
                if action == "clean":
                    self.assertFalse(os.path.lexists(target))
                elif action == "replaced":
                    self.assertEqual(target.read_bytes(), b"external replacement")
                else:
                    self.assertTrue(target.is_dir())
                    self.helper.apply(self.nix, plan, "fixture", digit * 32, tools, packed_cache=Cache(archive))
                    self.assertEqual(os.readlink(original), ".brook-profile-" + digit * 32)

    @unittest.skipUnless(shutil.which("nix-store"), "requires nix-store")
    def test_preflight_checks_guest_under_mutation_lock(self):
        plan = self.plan("7", "brook.calc", "Calculator", "selected-calc")
        capacity = {"block_bytes": 4096, "free_blocks": 16384, "free_bytes": 64 << 20,
                    "free_inodes": 0, "overlay_links": 2}

        def measure(*args):
            self.assertTrue((self.nix / "var/.brook-profile-lock").is_dir())
            return capacity

        with (patch.object(self.helper, "selected_capacity", side_effect=measure),
              patch.object(self.helper, "ensure_path") as import_path,
              self.assertRaisesRegex(ValueError, "Selected profile needs .* 0 inodes available")):
            self.helper.apply(self.nix, plan, "fixture", "7" * 32, {})
        import_path.assert_not_called()
        self.assertEqual(os.readlink(self.nix / legacy.PROFILE_ALIAS), ".brook-profile-legacy")

    @unittest.skipUnless(shutil.which("nix-store"), "requires nix-store")
    def test_prune_keeps_one_rollback_and_explicit_pins(self):
        home = self.nix / legacy.PROFILE_HOME
        generations = {digit: ".brook-profile-" + digit * 32 for digit in "2345"}
        for digit in generations:
            plan = self.plan(digit, "brook.calc", "Calculator", "selected-calc")
            with patch.object(self.helper.os, "sync"):
                self.helper.apply(self.nix, plan, "fixture", digit * 32, {})
        with self.helper.mutation_lock(self.nix):
            with self.assertRaisesRegex(ValueError, "Invalid profile transaction"):
                pin_generation(home, "../outside", True, self.helper.flush_directory)
            pin_generation(home, "3" * 32, True, self.helper.flush_directory)
            dry = prune_generations(home, generations["5"], self.helper.flush_directory, dry_run=True)
            self.assertEqual(dry["generations"], [generations["2"]])
            self.assertTrue((home / generations["2"]).is_dir())
            done = prune_generations(home, generations["5"], self.helper.flush_directory)
        self.assertEqual(done["generations"], [generations["2"]])
        self.assertFalse((home / generations["2"]).exists())
        for retained in (".brook-profile-legacy", generations["3"], generations["4"], generations["5"]):
            self.assertTrue((home / retained).is_dir())
        self.assertTrue((home / ".stage" / ("2" * 32 + ".json")).is_file())
        self.helper.rollback(self.nix, "5" * 32)
        self.assertEqual(os.readlink(self.nix / legacy.PROFILE_ALIAS), generations["4"])

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

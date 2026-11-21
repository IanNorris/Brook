#!/usr/bin/env python3
"""Check selected profile receipts and their signed stage recipe on the host."""

import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import image_profile
import stage_closure
from stage_nar import layout_digest


PROFILE = "1" * 32 + "-brook-selected-desktop"
CALCULATOR = "2" * 32 + "-wayland-calc"
EXTRA = "3" * 32 + "-htop"
STORE_PROFILE = Path("/nix/store") / PROFILE


class SelectedStageProfileTest(unittest.TestCase):
    def setUp(self):
        work = self.enterContext(tempfile.TemporaryDirectory())
        self.profile = Path(work) / PROFILE
        (self.profile / "share/brook").mkdir(parents=True)
        (self.profile / "share/applications").mkdir()
        self.launcher = self.profile / "share/applications/brook.calc.desktop"
        self.launcher.write_text("[Desktop Entry]\nName=Calculator\n")
        packages = [
            {"name": "wayland-calc", "version": "0.1", "store": CALCULATOR},
            {"name": "htop", "version": "3.5", "store": EXTRA},
        ]
        self.manifest = {
            "schema": "brook-profile/1",
            "preset": None,
            "redistributable": True,
            "launcherIds": ["brook.calc"],
            "packages": packages,
        }
        self.document = {
            "schema": image_profile.SCHEMA,
            "profile": str(STORE_PROFILE),
            "intelVulkanDriver": None,
            "manifest": self.manifest,
            "packages": packages,
        }
        self.receipt = Path(work) / "profile-selection.json"
        self.write_receipt()
        (self.profile / "share/brook/profile.json").write_text(json.dumps(self.manifest))

    def write_receipt(self):
        self.receipt.write_text(json.dumps(self.document))

    def resolve(self):
        # Use real receipt parsing and embedded-manifest comparison. Map its
        # exact expected store root to the disposable profile in this fixture.
        real_verify = image_profile.verify_realized_selection

        def verify(selection):
            with patch.object(image_profile, "store_path", return_value=self.profile):
                return real_verify(selection)

        with patch.object(stage_closure, "verify_realized_selection", side_effect=verify):
            return stage_closure.selected_profile(self.receipt, STORE_PROFILE)

    def test_verified_receipt_drives_existing_plan_format(self):
        profile, packages = self.resolve()
        self.assertEqual(profile, self.profile)
        entries = {
            PROFILE: {"references": sorted([CALCULATOR, EXTRA])},
            CALCULATOR: {"references": []},
            EXTRA: {"references": []},
        }
        layout = {
            "links": {
                "share/applications/brook.calc.desktop": f"/nix/store/{CALCULATOR}/calculator.desktop",
                "bin/htop": f"/nix/store/{EXTRA}/bin/htop",
            },
            "directories": ["bin", "share", "share/applications"],
        }
        with (patch.object(stage_closure, "closure_info", return_value=entries) as info,
              patch.object(stage_closure, "profile_layout", return_value=layout) as layout_from,
              patch.object(stage_closure, "upstream_has", return_value=False)):
            plan = stage_closure.generate_plan(STORE_PROFILE, [], None, False, packages)
        self.assertEqual(plan["format"], 2)
        self.assertEqual(plan["packages"], packages)
        info.assert_called_once_with([str(STORE_PROFILE), "/nix/store/" + CALCULATOR,
                                      "/nix/store/" + EXTRA])
        self.assertEqual(layout_from.call_args.args[0], STORE_PROFILE)
        self.assertEqual(plan["profile"], {"root": PROFILE, "layoutSha256": layout_digest(layout)})

    def test_default_stage_still_uses_network_expression(self):
        output = self.receipt.parent / "default-stage"
        launcher = {"closure": {}}
        argv = ["stage_closure.py", "--output", str(output), "--bridge-cache",
                "--cache-key", str(self.receipt), "--plan-key", str(self.receipt)]
        packages = self.document["packages"]
        build = SimpleNamespace(stdout=str(self.profile) + "\n")
        with (patch("sys.argv", argv),
              patch.object(stage_closure, "verify_private_key"),
              patch.object(stage_closure, "selected_profile") as selection,
              patch.object(stage_closure, "profile_metadata",
                           return_value={"profile": str(self.profile), "packages": packages}) as metadata,
              patch.object(stage_closure, "run", return_value=build) as run,
              patch.object(stage_closure, "generate_plan", return_value=launcher) as plan,
              patch.object(stage_closure, "publish_cache")):
            stage_closure.main()
        expression = stage_closure.EXPRESSION.resolve()
        selection.assert_not_called()
        metadata.assert_called_once_with(expression)
        plan.assert_called_once_with(self.profile, [], None, False, packages)
        self.assertEqual(run.call_args_list[0].args[0],
                         ["nix-build", str(expression), "-A", "profile", "--no-out-link"])

    def test_mismatched_requested_root_fails_before_realization(self):
        with patch.object(stage_closure, "verify_realized_selection") as verify:
            with self.assertRaisesRegex(ValueError, "does not match the requested store root"):
                stage_closure.selected_profile(self.receipt, Path("/nix/store") / CALCULATOR)
        verify.assert_not_called()

    def test_tampered_embedded_manifest_is_rejected(self):
        self.manifest["launcherIds"].append("com.example.Unselected")
        self.write_receipt()
        with self.assertRaisesRegex(ValueError, "differs from its embedded manifest"):
            self.resolve()

    def test_tampered_package_list_is_rejected(self):
        self.document["packages"] = self.document["packages"][:1]
        self.write_receipt()
        with self.assertRaisesRegex(ValueError, "package metadata differs"):
            self.resolve()

    def test_extra_desktop_entry_is_rejected(self):
        self.launcher.with_name("htop.desktop").write_text("[Desktop Entry]\nName=htop\n")
        with self.assertRaisesRegex(ValueError, "desktop files differ"):
            self.resolve()


if __name__ == "__main__":
    unittest.main()

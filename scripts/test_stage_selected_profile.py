#!/usr/bin/env python3
"""Check selected profile receipts and their signed stage recipe on the host."""

import json
from pathlib import Path
import tempfile
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import dell_deploy
import image_profile
import profile_cache_bundle
import stage_closure
from stage_nar import layout_digest
from stage_selected_space import layout_usage


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
        profile, packages, launchers = self.resolve()
        self.assertEqual(profile, self.profile)
        entries = {
            PROFILE: {"references": sorted([CALCULATOR, EXTRA]), "narSize": 1000},
            CALCULATOR: {"references": [], "narSize": 2000},
            EXTRA: {"references": [], "narSize": 3000},
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
              patch.object(stage_closure, "tree_inodes", return_value=4) as tree,
              patch.object(stage_closure, "upstream_has", return_value=False)):
            plan = stage_closure.generate_plan(STORE_PROFILE, [], None, False, packages,
                                                launcher_ids=launchers)
        self.assertEqual(plan["format"], 3)
        self.assertEqual(plan["packages"], packages)
        info.assert_called_once_with([str(STORE_PROFILE), "/nix/store/" + CALCULATOR,
                                      "/nix/store/" + EXTRA])
        self.assertEqual(layout_from.call_args.args[0], STORE_PROFILE)
        self.assertEqual(plan["profile"], {"root": PROFILE, "layoutSha256": layout_digest(layout),
                                           "launcherIds": ["brook.calc"], "space": layout_usage(layout)})
        self.assertEqual({name: entry["storeInodes"] for name, entry in plan["closure"].items()},
                         {name: 4 for name in entries})
        self.assertEqual(tree.call_count, 3)

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
        plan.assert_called_once_with(self.profile, [], None, False, packages, launcher_ids=None)
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


class SelectedDeploymentPreflightTest(unittest.TestCase):
    def run_stage(self, *, bridge_cache, capabilities, refused=None, bundle_size=0):
        work = Path(self.enterContext(tempfile.TemporaryDirectory()))
        plan = work / "plan"
        pair = work / "pair"
        plan.mkdir()
        pair.mkdir()
        document = plan / "stage-plan.json"
        signature = plan / "stage-plan.sig"
        layout = {"directories": ["bin"], "links": {"bin/tool": f"/nix/store/{PROFILE}/bin/tool"}}
        document.write_text(json.dumps({
            "format": 3,
            "cache": {"brook": None if bridge_cache else "http://example.invalid/"},
            "profile": {"launcherIds": [], "space": layout_usage(layout)},
            "closure": {PROFILE: {"storeInodes": 3, "narSize": 4 << 20}},
        }))
        signature.write_bytes(b"fake for preflight test")
        (pair / "build-manifest.json").write_text("{}")
        args = SimpleNamespace(output=work / "deployment", session="fixture", pair=pair,
                               profile_plan=plan, rollback_profile=None, push_profile_cache=False,
                               profile_timeout_s=10)
        bridge = Mock()
        bridge.load_session.return_value = {"host": "test", "port": 1234, "guest_boot_id": "fake"}
        journal = Mock(output=args.output / "profile")
        journal.receipt = {"profile_files": {
            path.name: dell_deploy.sha(path) for path in (document, signature)}}
        journal.wait_job.side_effect = [
            {"exit": {"code": 0}, "stdout": {"text": json.dumps(capabilities)}},
            {"exit": {"code": 0}},
        ]
        bulk = Mock()
        bulk.return_value.output = journal.output / "bulk"
        bulk.return_value.execute.side_effect = lambda *_, source, **__: {
            "transfer": {"sha256": dell_deploy.sha(source), "guest_path": f"/tmp/{source.name}"}}
        modules = {
            "hardware": SimpleNamespace(Hardware=Mock()),
            "hardware_bulk_workflow": SimpleNamespace(BulkRun=bulk),
            "hardware_update_workflow": SimpleNamespace(UpdateRun=Mock(return_value=journal)),
        }
        def pack_fixture(cache, recipe, directory):
            bundle = directory / "fixture.cache"
            with bundle.open("wb") as stream:
                stream.truncate(bundle_size)
            return [bundle]

        with (patch.dict(sys.modules, modules), patch.object(sys, "path", sys.path.copy()),
              patch.object(profile_cache_bundle, "pack_cache", side_effect=pack_fixture)):
            if refused is None and capabilities.get("selected_profile_api") != 1:
                refused = "Refresh the guest stage helper"
            if refused:
                with self.assertRaisesRegex(ValueError, refused):
                    dell_deploy.stage_profile(args, bridge=bridge)
            else:
                self.assertIs(dell_deploy.stage_profile(args, bridge=bridge), journal)
        return bridge, bulk, journal

    def test_old_helper_rejected_before_bulk_for_both_transports(self):
        for push in (False, True):
            with self.subTest(bridge_cache=push):
                bridge, bulk, _ = self.run_stage(bridge_cache=push, capabilities={"bridge_cache_api": 1})
                bulk.assert_not_called()
                bridge.guest_run.assert_called_once()
                self.assertEqual(bridge.guest_run.call_args.args[2][-1], "cache-capabilities")

    @staticmethod
    def capacity(free_blocks=32 * 1024, free_inodes=32768, *, present=True, shared=True):
        return {"selected_profile_api": 1,
                "selected_store": {"block_bytes": 4096, "free_blocks": free_blocks,
                                   "free_bytes": 4096 * free_blocks, "free_inodes": free_inodes,
                                   "overlay_links": 1, "store_names": [PROFILE] if present else [],
                                   "transfer_shares_space": shared}}

    def test_http_transport_probes_before_verified_apply(self):
        bridge, bulk, journal = self.run_stage(bridge_cache=False, capabilities=self.capacity())
        self.assertEqual(bulk.call_count, 2)
        self.assertEqual([call.args[2][2] for call in bridge.guest_run.call_args_list],
                         ["cache-capabilities", "apply"])
        self.assertEqual(journal.save.call_args.kwargs["state"], "profile_verified")
        checks = [call.kwargs for call in journal.save.call_args_list
                  if call.kwargs.get("state") == "selected_profile_preflight_verified"]
        self.assertEqual(checks[0]["selected_profile_required"]["missing_store_paths"], 0)

    def test_inode_and_block_shortage_refuse_before_transfer(self):
        for capacity in (self.capacity(free_inodes=0), self.capacity(free_blocks=0)):
            with self.subTest(capacity=capacity):
                bridge, bulk, _ = self.run_stage(
                    bridge_cache=False, capabilities=capacity, refused="Selected profile needs .* available")
                bridge.guest_run.assert_called_once()
                bulk.assert_not_called()

    def test_push_reserves_transfer_bytes_on_shared_volume(self):
        cap = self.capacity(free_blocks=5000)
        cap.update(bridge_cache_api=1, block_bytes=4096, free_bytes=5000 * 4096)
        bridge, bulk, _ = self.run_stage(bridge_cache=True, capabilities=cap, bundle_size=8 << 20,
                                         refused="Selected profile needs .* available")
        bridge.guest_run.assert_called_once()
        bulk.assert_not_called()
        cap["selected_store"]["transfer_shares_space"] = False
        _, bulk, _ = self.run_stage(bridge_cache=True, capabilities=cap, bundle_size=8 << 20)
        self.assertEqual(bulk.call_count, 3)

    def test_missing_paths_charge_archive_and_store_bytes(self):
        cap = self.capacity(free_blocks=5000, present=False)
        bridge, bulk, _ = self.run_stage(
            bridge_cache=False, capabilities=cap, refused="Selected profile needs .* available")
        bridge.guest_run.assert_called_once()
        bulk.assert_not_called()
        cap["selected_store"]["store_names"] = [PROFILE]
        _, bulk, _ = self.run_stage(bridge_cache=False, capabilities=cap)
        self.assertEqual(bulk.call_count, 2)


if __name__ == "__main__":
    unittest.main()

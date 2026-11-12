"""Offline fault cases for the Dell recovery controller."""

import contextlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from dell_recovery import AC_OFF_SECONDS, Journal, OUTAGE_SECONDS, Recovery, read_caps
from dell_recovery_lock import inhibit_recovery, maintenance_lock, state_directory


HOST = "192.0.2.10"
BOOT = "1" * 32
NEW_BOOT = "2" * 32


class RecoveryTest(unittest.TestCase):
    def setUp(self):
        temporary = self.enterContext(tempfile.TemporaryDirectory())
        self.enterContext(patch.dict(os.environ, {"BROOK_HARDWARE_REGISTRY": temporary}))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.config = Path(temporary) / "power.txt"
        directory = state_directory(HOST, 1234)
        directory.mkdir(parents=True)
        self.journal = Journal(directory)
        self.now = 0
        self.actions = []
        self.boot = BOOT
        self.ac = "ON"
        self.ready = True
        self.desktop_ready = True
        self.shutdown_record = None
        self.payload = "dell-approved"
        self.lose_off_reply = False
        self.fail_restore = False
        self.wake_boot = NEW_BOOT
        self.wake_payload = self.payload
        self.recovery = self.controller()

    def controller(self):
        return Recovery(self.journal, HOST, 1234, self.config, clock=lambda: self.now,
                        sleep=self.sleep, caps=self.caps, switch=self.relay,
                        diagnose=lambda *args, **kwargs: {"bridge": {}})

    def sleep(self, seconds):
        self.now += seconds

    def caps(self, host, port):
        if self.boot is not None:
            return {"boot_id": self.boot, "payload_id": self.payload,
                    "ac_recovery": {"api": 1, "ready": self.ready,
                                    "source": "online"},
                    "writable_desktop": {"ready": self.desktop_ready, "state_uncertain": False},
                    "power": {"power_off": True}, "ac_shutdown_record": self.shutdown_record}
        return None

    def relay(self, config, action):
        self.actions.append((self.now, action))
        if action == "off":
            self.ac = "OFF"
            if self.lose_off_reply:
                return None
        if action == "on":
            if self.fail_restore:
                return None
            self.ac = "ON"
            self.boot = self.wake_boot
            self.payload = self.wake_payload
        return self.ac

    def outage(self):
        self.boot = None
        self.assertIsNone(self.recovery.poll())
        self.sleep(OUTAGE_SECONDS)

    def test_unready_guest_cannot_arm(self):
        self.ready = False
        with self.assertRaises(RuntimeError):
            self.recovery.arm()
        self.outage()
        self.assertIsNone(self.recovery.poll())
        self.assertEqual(self.actions, [])

    def test_ready_ac_during_desktop_boot_cannot_arm(self):
        self.desktop_ready = False
        with self.assertRaises(RuntimeError):
            self.recovery.arm()
        self.assertEqual(self.actions, [])

    def test_ambiguous_off_is_reconciled_once(self):
        self.recovery.arm()
        self.lose_off_reply = True
        self.outage()
        self.assertEqual(self.recovery.poll(), 0)
        changes = [(when, action) for when, action in self.actions if action != "status"]
        self.assertEqual(changes, [(OUTAGE_SECONDS, "off"), (OUTAGE_SECONDS + AC_OFF_SECONDS, "on")])
        self.assertEqual(self.journal.load()["outcome"], "wake_verified")
        self.assertEqual(self.journal.load()["shutdown_finalization"], {"status": "unavailable"})
        self.assertEqual(self.controller().run(), 1)
        self.assertEqual(self.actions.count((OUTAGE_SECONDS, "off")), 1)

    def test_durable_shutdown_record_is_correlated_with_armed_boot(self):
        self.recovery.arm()
        self.shutdown_record = {"stage": "checked_finalization", "sequence": 7, "boot_id": BOOT}
        self.outage()
        self.assertEqual(self.recovery.poll(), 0)
        self.assertEqual(self.journal.load()["shutdown_finalization"],
                         {"status": "matched", "sequence": 7, "boot_id": BOOT})

    def test_stale_record_does_not_prove_finalization(self):
        self.recovery.arm()
        self.shutdown_record = {"stage": "checked_finalization", "sequence": 7, "boot_id": "3" * 32}
        self.outage()
        self.assertEqual(self.recovery.poll(), 0)
        self.assertEqual(self.journal.load()["shutdown_finalization"]["status"], "different_boot")

    def test_restart_after_off_dispatch_restores_without_resending_off(self):
        self.recovery.arm()
        self.recovery.record("switching_off", host_boot=self.recovery.host_boot)
        self.ac = "OFF"
        self.assertEqual(self.controller().run(), 0)
        self.assertNotIn("off", [action for _, action in self.actions])
        self.assertIn((AC_OFF_SECONDS, "on"), self.actions)

    def test_journal_error_after_off_still_restores_power(self):
        self.recovery.arm()
        self.outage()
        save = self.journal.save

        def fail_after_off(state):
            if state["state"] == "waiting_off":
                raise OSError("journal volume failed")
            save(state)

        with patch.object(self.journal, "save", side_effect=fail_after_off):
            with self.assertRaises(OSError):
                self.recovery.poll()
        self.assertEqual(self.ac, "ON")
        self.assertEqual(self.journal.load()["state"], "switching_off")
        self.assertEqual(sum(action == "off" for _, action in self.actions), 1)

    def test_same_boot_is_not_recovery_success(self):
        self.recovery.arm()
        self.wake_boot = BOOT
        self.outage()
        self.assertEqual(self.recovery.poll(), 1)
        self.assertEqual(self.journal.load()["outcome"], "same_boot_reachable")
        self.assertEqual(self.ac, "ON")

    def test_changed_boot_on_rollback_payload_is_not_verified(self):
        self.recovery.arm()
        self.wake_payload = "dell-rollback"
        self.outage()
        self.assertEqual(self.recovery.poll(), 1)
        self.assertEqual(self.journal.load()["outcome"], "unexpected_payload")
        self.assertEqual(self.journal.load()["observed_payload_id"], "dell-rollback")

    def test_failed_restore_remains_resumable(self):
        self.recovery.arm()
        self.fail_restore = True
        self.outage()
        self.assertEqual(self.recovery.poll(), 1)
        self.assertEqual(self.journal.load()["state"], "restore_failed")
        with self.assertRaises(RuntimeError), inhibit_recovery(HOST, 1234):
            self.fail("Maintenance must remain inhibited while AC restoration is pending")
        self.fail_restore = False
        self.assertEqual(self.controller().run(), 0)
        self.assertEqual(sum(action == "off" for _, action in self.actions), 1)

    def test_maintenance_invalidates_arm_even_between_polls(self):
        self.recovery.arm()
        with inhibit_recovery(HOST, 1234):
            with self.assertRaises(BlockingIOError), maintenance_lock(HOST, 1234, nonblocking=True):
                self.fail("Recovery cannot overlap maintenance")
        self.boot = None
        self.assertEqual(self.recovery.poll(), 1)
        self.assertEqual(self.journal.load()["outcome"], "maintenance_invalidated_arm")
        self.assertEqual(self.actions, [(0, "status")])

    def test_probe_pins_a_fresh_boot_before_verifying(self):
        blank = {"hardware_api": 1, "boot_guard": True, "boot_id": "0" * 32}
        pinned = {**blank, "boot_id": NEW_BOOT}
        with patch("dell_recovery.exchange", side_effect=[(blank,), ({"boot_id": NEW_BOOT}, pinned)]) as exchange:
            self.assertEqual(read_caps(HOST, 1234), pinned)
            self.assertTrue(exchange.call_args.args[2][0].startswith("bridge epoch init "))


if __name__ == "__main__":
    unittest.main()

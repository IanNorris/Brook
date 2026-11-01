#!/usr/bin/env python3
import hashlib
import lzma
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from dell_linux_audio_snapshot import snapshot


class AudioReferenceTests(unittest.TestCase):
    def test_missing_devices_are_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            report = snapshot(Path(directory))
            self.assertEqual(report["acpi"]["devices"], [])
            self.assertIn("error", report["acpi"])
            self.assertIsNone(report["kernel_messages"])
            self.assertIn("error", report["sound_cards"])

    def test_fixture_includes_only_audio_devices_and_filtered_messages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            acpi = root / "sys/bus/acpi/devices/INT3438:00"
            acpi.mkdir(parents=True)
            (acpi / "status").write_text("15\n")
            (acpi / "modalias").write_text("acpi:INT3438:\n")
            platform = root / "sys/bus/platform/devices/INT3438:00"
            platform.mkdir(parents=True)
            (platform / "resource").write_text("0x1000 0x1fff 0x200\n")
            (acpi / "physical_node").symlink_to(platform)
            (platform / "driver").symlink_to("/sys/bus/platform/drivers/catpt")
            machine = root / "sys/bus/platform/devices/broadwell-audio"
            machine.mkdir()
            (machine / "driver").symlink_to("/sys/bus/platform/drivers/broadwell-audio")
            codec = root / "sys/bus/i2c/devices/i2c-INT343A:00"
            codec.mkdir(parents=True)
            (codec / "driver").symlink_to("/sys/bus/i2c/drivers/rt286")
            (codec / "modalias").write_text("acpi:INT343A:\n")
            numeric_codec = root / "sys/bus/i2c/devices/2-001c"
            numeric_codec.mkdir()
            (numeric_codec / "driver").symlink_to("/sys/bus/i2c/drivers/rt286")
            unrelated_i2c = root / "sys/bus/i2c/devices/1-0040"
            unrelated_i2c.mkdir()
            (unrelated_i2c / "driver").symlink_to("/sys/bus/i2c/drivers/unrelated")
            card = root / "sys/class/sound/card1"
            card.mkdir(parents=True)
            (card / "id").write_text("broadwellrt286\n")
            (card / "device").symlink_to(machine)
            other = root / "sys/bus/acpi/devices/UNRELATED:00"
            other.mkdir()
            tables = root / "sys/firmware/acpi/tables"
            tables.mkdir(parents=True)
            (tables / "DSDT").write_bytes(b"test table")
            firmware = root / "lib/firmware/intel"
            firmware.mkdir(parents=True)
            (firmware / "IntcSST2.bin.xz").write_bytes(lzma.compress(b"firmware"))
            report = snapshot(root, "unrelated private text\ncatpt: initialized\ni2c_designware: ready\nsnd_soc_rt286: ready\n")
            self.assertEqual(len(report["acpi"]["devices"]), 1)
            node = report["acpi"]["devices"][0]["physical_nodes"][0]
            self.assertEqual(node["driver"], "catpt")
            self.assertIn("0x1000", node["resource"]["text"])
            self.assertEqual(report["firmware_tables"]["DSDT"]["sha256"], hashlib.sha256(b"test table").hexdigest())
            self.assertEqual(report["kernel_messages"], ["catpt: initialized", "i2c_designware: ready", "snd_soc_rt286: ready"])
            self.assertIn("error", node["irq"])
            self.assertEqual([d["name"] for d in report["platform"]["devices"]], ["INT3438:00", "broadwell-audio"])
            self.assertEqual([d["driver"] for d in report["i2c"]["devices"]], ["rt286", "rt286"])
            self.assertEqual(report["sound_card_devices"]["cards"][0]["device"], "broadwell-audio")
            self.assertEqual(report["sound_card_devices"]["cards"][0]["driver"], "broadwell-audio")
            self.assertEqual(report["firmware"]["intel/IntcSST2.bin"]["payload_sha256"], hashlib.sha256(b"firmware").hexdigest())

    def test_limits_and_truncation_are_visible(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            firmware = root / "lib/firmware/intel"
            firmware.mkdir(parents=True)
            (firmware / "IntcSST2.bin.xz").write_bytes(lzma.compress(b"A" * (4 * 1024 * 1024 + 1)))
            report = snapshot(root, "catpt: message\n" * 201)
            self.assertIn("payload_error", report["firmware"]["intel/IntcSST2.bin"])
            self.assertTrue(report["kernel_messages_truncated"])
            self.assertEqual(len(report["kernel_messages"]), 200)

    def test_cli_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "snapshot.json"
            output.write_text("keep")
            script = Path(__file__).with_name("dell_linux_audio_snapshot.py")
            result = subprocess.run([sys.executable, str(script), "--output", str(output)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_text(), "keep")

    def test_xz_trailing_stream_and_dictionary_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            firmware = root / "lib/firmware/intel"
            firmware.mkdir(parents=True)
            path = firmware / "IntcSST2.bin.xz"
            path.write_bytes(lzma.compress(b"a") + lzma.compress(b"b"))
            self.assertIn("payload_error", snapshot(root)["firmware"]["intel/IntcSST2.bin"])
            path.write_bytes(lzma.compress(b"small", filters=[{"id": lzma.FILTER_LZMA2, "dict_size": 64 * 1024 * 1024}]))
            self.assertIn("error", snapshot(root)["firmware"]["intel/IntcSST2.bin"])


if __name__ == "__main__":
    unittest.main()

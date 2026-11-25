#!/usr/bin/env python3
"""Unit tests for nv_gsp_trace_diff.py (synthetic data only)."""
import importlib.util
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


td = load("nv_gsp_trace_diff")


def raw(op, off, val, phase="fwsec_frts", step="boot"):
    return {"op": op, "off": off, "val": val, "phase": phase, "step": step}


class TraceDiffTest(unittest.TestCase):
    def setUp(self):
        self.dec = td.load_decoder()
        self.log = [
            raw("W", 0x110040, 0), raw("W", 0x110104, 0xfd00), raw("W", 0x110100, 2),
            raw("R", 0x110100, 0), raw("R", 0x110100, 0), raw("R", 0x110100, 0x10),
            raw("W", 0x110180, 0x0100fe00, step="load"),
            raw("W", 0x110188, 0xfd, step="load"), raw("W", 0x110184, 0x11223344, step="load"),
            raw("W", 0x840040, 0x0c000000, "booter_load"), raw("W", 0x840044, 1, "booter_load"),
        ]

    def test_identical_and_masked(self):
        ref = td.normalise_brook(self.dec, self.log)
        mine = list(self.log)
        mine[9] = raw("W", 0x840040, 0x0d000000, "booter_load")   # another DMA address: masked
        mine[4] = raw("R", 0x110100, 0)                             # poll count differs: ignored
        del mine[3]
        d = td.diff(ref, td.normalise_brook(self.dec, mine))
        self.assertEqual(d["totals"], {"equal": len(ref), "ref_only": 0, "brook_only": 0, "changed": 0})
        pio = [r for r in ref if r["op"] == "PIO"]
        self.assertEqual(len(pio), 1)
        self.assertEqual(pio[0]["tag"], "0x000000fd")

    def test_changes_are_reported(self):
        ref = td.normalise_brook(self.dec, self.log)
        mine = list(self.log)
        mine[1] = raw("W", 0x110104, 0)                       # different BOOTVEC
        mine[8] = raw("W", 0x110184, 0x55667788, step="load")  # different IMEM content
        mine.append(raw("R", 0x1fa828, 0x01fffc00, "booter_load", "brook_wpr2_check"))
        d = td.diff(ref, td.normalise_brook(self.dec, mine))
        self.assertEqual(d["totals"]["changed"], 2)
        self.assertEqual(d["totals"]["brook_only"], 1)
        md = td.report_md(d, "ref", "brook")
        self.assertIn("brook_wpr2_check", md)

    def test_extract_seq(self):
        payload = bytes(range(40)) + bytes(4 * 3)
        lines = ["[  160.715551] nouveau 0000:02:00.0: gsp:msg fn:4098 len:0x%x/0x%x res:0x0 resp:0x0"
                 % (len(payload) + 32, len(payload))]
        for off in range(0, len(payload), 16):
            chunk = payload[off:off + 16]
            lines.append("[  160.715554] msg: %08x: %s  ................" %
                         (off, " ".join("%02x" % b for b in chunk) + " "))
        lines.append("[  160.715650] nouveau 0000:02:00.0: gsp: seq: 00003fe2 000001ea")
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("\n".join(lines) + "\n")
            path = f.name
        try:
            self.assertEqual(td.extract_seq(path), payload)
            with self.assertRaises(ValueError):
                td.extract_seq(path, fn=4097)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()

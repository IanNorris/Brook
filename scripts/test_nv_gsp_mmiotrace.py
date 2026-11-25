#!/usr/bin/env python3
"""Synthetic tests for nv_gsp_mmiotrace.py (no capture data needed)."""
import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nv_gsp_mmiotrace as m  # noqa: E402

BAR0 = 0xde000000


def line(op, ts, off, val):
    return "%s 4 %.6f 1 0x%x 0x%x 0x0 0\n" % (op, ts, BAR0 + off, val)


class Decoder(unittest.TestCase):
    def run_trace(self, lines, dmesg=None):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "t.txt")
            with open(p, "w") as f:
                f.write("VERSION 20070824\n")
                f.write("UNKNOWN 1.0 56 0xc0200000 f3,a5,f6 0x0 0\n")
                f.writelines(lines)
            dp = None
            if dmesg:
                dp = os.path.join(d, "dmesg.txt")
                with open(dp, "w") as f:
                    f.write(dmesg)
            recs, stats, _ = m.build(p, dp)
            out = os.path.join(d, "o.jsonl")
            m.write_jsonl(recs, out)
            with open(out) as f:
                rows = [json.loads(x) for x in f]
        return rows, stats

    def fwsec_lines(self):
        t, L = 1.0, []

        def add(op, off, val):
            nonlocal t
            t += 0.000001
            L.append(line(op, t, off, val))
        add("R", 0x000000, 0x164000a1)
        add("R", 0x625f04, 0x20e09)
        add("W", 0x110c00, 0)                 # queued RPC doorbell
        add("R", 0x11012c, 0xca101136)        # acquire
        add("R", 0x110048, 0x1004)            # reset
        add("W", 0x110048, 0x1004)
        add("R", 0x009410, 0x1234)            # PTIMER: excluded, must not split poll
        for v in (7, 7, 1):
            add("R", 0x11010c, v)
            add("R", 0x009400, 0x1)
        add("R", 0x110624, 0x110)             # load
        add("W", 0x110624, 0x190)
        add("W", 0x110180, 0x0100fe00)
        add("W", 0x110188, 0xfd)
        for w in range(64):
            add("W", 0x110184, w)
        add("W", 0x110188, 0xfe)
        for w in range(64):
            add("W", 0x110184, 0x100 + w)
        add("W", 0x1101c0, 0x01000000)
        desc = [0] * 8 + [4, 0x09d30000, 1, 0, 0x300, 0x300, 0x9a00, 0,
                          0x09d39d00, 1, 0x410, 0, 0]
        for w in desc:
            add("W", 0x1101c4, w)
        add("W", 0x110040, 0)                 # boot
        add("W", 0x110104, 0xfd00)
        add("W", 0x110100, 2)
        for v in (0, 0, 0, 0x10):
            add("R", 0x110100, v)
        add("R", 0x001438, 0)                 # verify
        add("R", 0x1fa824, 0x01ffee00)
        add("R", 0x1fa828, 0x01fffc00)
        add("R", 0x1103c0, 0)                 # riscv reset
        add("W", 0x1103c0, 1)
        add("W", 0x1103c0, 0)
        add("W", 0x110040, 0x12a67000)
        add("W", 0x110044, 1)
        add("R", 0x84012c, 0xca101136)        # booter acquire
        add("W", 0x840040, 0x12395000)
        add("W", 0x840044, 1)
        return L, desc

    def test_phases_polls_pio_masking(self):
        L, desc = self.fwsec_lines()
        rows, stats = self.run_trace(L)
        self.assertEqual(stats["unknown"], 1)
        self.assertEqual(stats["excluded"].get("ptimer"), 4)
        phases = [r["phase"] for r in rows]
        self.assertEqual(phases[0], "pre")
        self.assertEqual(rows[2]["step"], "cmdq_doorbell")
        # Polling collapse across excluded timer reads.
        dmactl = [r for r in rows if r["reg"] == "0x11010c"]
        self.assertEqual(len(dmactl), 1)
        self.assertEqual((dmactl[0]["op"], dmactl[0]["count"], dmactl[0]["first"],
                          dmactl[0]["final"], dmactl[0]["val"]),
                         ("POLL", 3, "0x00000007", "0x00000001", "0x00000001"))
        cpu = [r for r in rows if r["reg"] == "0x110100" and r["op"] == "POLL"][0]
        self.assertEqual((cpu["count"], cpu["final"], cpu["step"]), (4, "0x00000010", "boot"))
        # IMEM PIO: one record spanning both tags.
        imem = [r for r in rows if r["op"] == "PIO" and r["port"] == "imem"]
        self.assertEqual(len(imem), 1)
        words = list(range(64)) + [0x100 + w for w in range(64)]
        blob = b"".join(struct.pack("<I", w) for w in words)
        self.assertEqual((imem[0]["start"], imem[0]["tag"], imem[0]["bytes"]),
                         ("0x0000fe00", "0x000000fd", 512))
        self.assertEqual(imem[0]["sha256"], hashlib.sha256(blob).hexdigest())
        self.assertEqual(imem[0]["sha256"], imem[0]["sha256_masked"])
        # DMEM bootloader descriptor: DMA words masked.
        dm = [r for r in rows if r["op"] == "PIO" and r["port"] == "dmem"][0]
        self.assertEqual(dm["masked_words"], [9, 10, 16, 17])
        md = list(desc)
        for i in (9, 10, 16, 17):
            md[i] = 0
        self.assertEqual(dm["sha256_masked"],
                         hashlib.sha256(b"".join(struct.pack("<I", w) for w in md)).hexdigest())
        self.assertNotEqual(dm["sha256"], dm["sha256_masked"])
        # Phase/steps and the verify reads.
        by = {(r["reg"], r["op"]): r for r in rows}
        self.assertEqual(by[("0x001438", "R")]["step"], "verify")
        self.assertEqual(by[("0x1fa824", "R")]["note"], "addr 0x1ffee0000")
        self.assertEqual(by[("0x1fa828", "R")]["note"], "addr 0x1fffc0000")
        lib = [r for r in rows if r["phase"] == "gsp_riscv_reset" and r["op"] == "W"
               and r["reg"] in ("0x110040", "0x110044")]
        self.assertEqual([r["val"] for r in lib], ["<dma:libos_lo>", "<dma:libos_hi>"])
        self.assertEqual(lib[0]["step"], "libos_args")
        wm = [r for r in rows if r["phase"] == "booter_load" and r["op"] == "W"]
        self.assertEqual([r["val"] for r in wm], ["<dma:wpr_meta_lo>", "<dma:wpr_meta_hi>"])
        # FWSEC boot mbox0 = 0 is a single write and stays unmasked.
        fb = [r for r in rows if r["phase"] == "fwsec_frts" and r["reg"] == "0x110040"]
        self.assertEqual(fb[0]["val"], "0x00000000")
        # Schema key order.
        self.assertEqual(list(rows[0].keys())[:10],
                         ["i", "ts", "phase", "step", "op", "reg", "val", "block", "name", "note"])

    def test_dmesg_doorbell_annotation(self):
        L = [line("R", 1.0, 0, 0x164000a1), line("W", 1.1, 0x110c00, 0)]
        dm = ("[    0.900000] nouveau 0000:02:00.0: gsp: rpc fn:72 len:0x3c0/0x3a0\n"
              "[    0.900001] nouveau 0000:02:00.0: gsp: cmdq: wptr 1\n")
        rows, _ = self.run_trace(L, dm)
        self.assertEqual(rows[-1]["note"], "cmdq doorbell, wptr 1, rpc fn 72")


if __name__ == "__main__":
    unittest.main()

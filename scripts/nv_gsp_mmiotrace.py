#!/usr/bin/env python3
"""Decode a Linux mmiotrace of nouveau booting GSP-RM (TU10x, r570) into an
annotated, collapsed reference sequence (JSONL + Markdown summary).

Stdlib only; run with `python3 -I`.  The trace and dmesg are treated as data.

    python3 -I scripts/nv_gsp_mmiotrace.py TRACE[.zst] --dmesg dmesg-final.txt \
        --jsonl out.jsonl --md out.md

Model (nouveau, drivers/gpu/drm/nouveau, Linux 7.3/7.4):
  pre            probes before the GSP falcon is acquired; the queued
                 SET_SYSTEM_INFO/SET_REGISTRY doorbells (r535/rpc.c cmdq_push)
  fwsec_frts     gsp/fwsec.c nvkm_gsp_fwsec_frts: acquire, reset
                 (falcon/gm200.c disable/enable, gp102.c reset_eng 0x3c0),
                 load (gm200_flcn_fw_load + tu102_gsp_fwsec_load_bld),
                 boot (gm200_flcn_fw_boot), verify (0x1438, 0x1fa824/828)
  gsp_riscv_reset tu102_gsp_oneinit tail: reset_eng, 0x040/0x044 = libos args
  booter_load    tu102_gsp_booter_load on SEC2 (mbox0/1 = WPR meta address)
  gsp_handshake  r535_gsp_init: 0x080 = app version, RISC-V active (0x1240)
  cpu_sequencer  r535_gsp_msg_run_cpu_sequencer replay (dmesg-guided steps)
  init_done_wait r535_gsp_rpc_poll(GSP_INIT_DONE): sysmem msgq, no MMIO
  postinit       GET_GSP_STATIC_INFO + ctrl RPC doorbells, intr table,
                 0x110004 = 0x40 (r535_gsp_postinit); then the first few
                 later cmdq doorbells (GSP block only)
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

BAR0_DEFAULT = 0xde000000
BAR0_SIZE = 0x1000000

GSP = 0x110000
SEC2 = 0x840000

FALCON_REGS = {
    0x000: "IRQSSET", 0x004: "IRQSCLR", 0x008: "IRQSTAT", 0x010: "IRQMSET",
    0x014: "IRQMCLR", 0x01c: "IRQDEST", 0x040: "MAILBOX0", 0x044: "MAILBOX1",
    0x048: "ITFEN", 0x04c: "IDLESTATE", 0x054: "INST_BIND", 0x058: "INST_CTL",
    0x080: "OS", 0x084: "ENGCTL_BOOT0", 0x090: "DEBUG1", 0x0a4: "IDLE",
    0x0dc: "BIND_STAT", 0x100: "CPUCTL", 0x104: "BOOTVEC", 0x108: "HWCFG",
    0x10c: "DMACTL", 0x110: "DMATRFBASE", 0x12c: "HWCFG1", 0x130: "CPUCTL_ALIAS",
    0x148: "TRACEIDX", 0x14c: "TRACEPC",
    0x180: "IMEMC", 0x184: "IMEMD", 0x188: "IMEMT",
    0x1c0: "DMEMC", 0x1c4: "DMEMD", 0x240: "SCTL", 0x3c0: "ENGINE_RESET",
    0x408: "DEBUG (SEC2: bit20 debug-fused)", 0x624: "FBIF_CTL", 0xc00: "QUEUE_HEAD0", 0xc04: "QUEUE_HEAD1",
}
for _i in range(8):
    FALCON_REGS[0x600 + 4 * _i] = "FBIF_TRANSCFG%d" % _i
RISCV_REGS = {0x240: "RISCV_CPUCTL", 0x2b4: "RISCV_IRQMASK", 0x2b8: "RISCV_IRQDEST"}

NAMED = {
    0x000000: ("pmc", "PMC_BOOT_0"), 0x000004: ("pmc", "PMC_BOOT_1"),
    0x000200: ("pmc", "PMC_ENABLE"), 0x101000: ("strap", "PEXTDEV_BOOT_0"),
    0x001438: ("pbus", "SW_SCRATCH_0E (FWSEC-FRTS err bits 31:16)"),
    0x001454: ("pbus", "SW_SCRATCH_15 (FWSEC-SB err bits 15:0)"),
    0x1fa824: ("fb", "WPR2_ADDR_LO"), 0x1fa828: ("fb", "WPR2_ADDR_HI"),
    0x100ce0: ("fb", "MMU_LOCAL_MEMORY_RANGE (FB size)"),
    0x100c10: ("fb", "MMU_FLUSH_SYSMEM_ADDR"),
    0x118128: ("gc6", "devinit status (bit0 done)"),
    0x118234: ("gc6", "devinit scratch (0xff/0x3ff done)"),
    0x1180f8: ("gc6", "SEC2 boot status (bit26 GSP-RM lockdown released)"),
    0x625f04: ("disp", "VGA_WORKSPACE_BASE (bit3 valid)"),
    0x021c00: ("fuse", "FUSE 0x21c00"), 0x021c04: ("fuse", "FUSE 0x21c04"),
    0x088050: ("pci_cfg", "PCI cfg mirror 0x50"), 0x088704: ("pci_cfg", "PCI cfg mirror 0x704"),
    0x070000: ("bar", "BAR_FLUSH"), 0x6013d4: ("vga", "VGA_CRTC_INDEX"),
    0x6013d5: ("vga", "VGA_CRTC_DATA"), 0x001700: ("pramin", "PRAMIN window"),
}


def classify(off):
    """(block, name) for a BAR0 offset, or (None, None) for excluded regions."""
    if off in NAMED:
        return NAMED[off]
    for base, blk in ((GSP, "gsp"), (SEC2, "sec2")):
        if base <= off < base + 0x1000:
            r = off - base
            return blk, FALCON_REGS.get(r, "FLCN_%03x" % r)
        if base + 0x1000 <= off < base + 0x2000:
            r = off - base - 0x1000
            return blk + "_riscv", RISCV_REGS.get(r, "RISCV_%03x" % r)
    if 0x009000 <= off < 0x00a000:
        return None, "ptimer"
    if 0x300000 <= off < 0x400000:
        return None, "prom"
    if 0x700000 <= off < 0x800000:
        return None, "pramin"
    if 0x610000 <= off < 0x700000:
        return None, "disp"
    if off < 0x001000:
        return "pmc", "PMC_%03x" % off
    if 0x001400 <= off < 0x001500:
        return "pbus", "SW_SCRATCH_%02x" % ((off - 0x1400) // 4)
    if 0xb81000 <= off < 0xb82000:
        return "vfn_intr", "VFN_INTR_%03x" % (off - 0xb81000)
    if 0xb80000 <= off < 0xbc0000:
        return None, "vfn_other"
    if 0x100000 <= off < 0x200000:
        return "fb", "FB_%06x" % off
    if 0x118000 <= off < 0x119000:
        return "gc6", "GC6_%06x" % off
    if 0x021000 <= off < 0x022000:
        return "fuse", "FUSE_%06x" % off
    if 0x088000 <= off < 0x089000:
        return "pci_cfg", "PCICFG_%03x" % (off - 0x88000)
    return "other", "REG_%06x" % off


def open_text(path):
    if path.endswith(".zst"):
        p = subprocess.Popen(["zstd", "-dc", path], stdout=subprocess.PIPE)
        return p.stdout, p
    return open(path, "rb"), None


def parse_trace(path, bar0=BAR0_DEFAULT):
    """Return (accesses, stats). Access = dict(op, ts, off, val)."""
    acc, stats = [], {"lines": 0, "unknown": 0, "bar0_rw": 0, "other_bar_rw": 0,
                      "excluded": {}, "maps": 0, "marks": []}
    f, proc = open_text(path)
    try:
        for raw in f:
            stats["lines"] += 1
            line = raw.decode("ascii", "replace").split()
            if not line:
                continue
            t = line[0]
            if t in ("R", "W"):
                # R width ts map phys value pc pid
                phys = int(line[4], 16)
                if not (bar0 <= phys < bar0 + BAR0_SIZE):
                    stats["other_bar_rw"] += 1
                    continue
                stats["bar0_rw"] += 1
                acc.append({"op": t, "ts": float(line[2]), "off": phys - bar0,
                            "val": int(line[5], 16), "w": int(line[1])})
            elif t == "UNKNOWN":
                stats["unknown"] += 1
            elif t == "MAP":
                stats["maps"] += 1
            elif t == "MARK":
                stats["marks"].append(" ".join(line[1:]))
    finally:
        f.close()
        if proc:
            proc.wait()
    return acc, stats


DMESG_RE = re.compile(r"^\[\s*([0-9.]+)\]\s+(.*)$")


def parse_dmesg(path):
    out = []
    if not path:
        return out
    with open(path, "rb") as f:
        for raw in f:
            m = DMESG_RE.match(raw.decode("utf-8", "replace").rstrip("\n"))
            if m:
                out.append((float(m.group(1)), m.group(2)))
    return out


def dmesg_info(dm):
    """Anchors, sequencer ops and the rpc fn behind each cmdq doorbell."""
    info = {"anchors": {}, "seq": [], "doorbells": [], "init_done": None,
            "init_completed": None, "wpr2": None, "mbox": {}}
    last_rpc = None
    for ts, msg in dm:
        if "nouveau" not in msg:
            continue
        body = msg.split(": ", 1)[-1]
        if "gsp:fwsec-frts: booting" in msg:
            info["anchors"].setdefault("fwsec_boot", ts)
        elif "booter-load: booting" in msg:
            info["anchors"].setdefault("booter_boot", ts)
        m = re.search(r"gsp: rpc fn:(\d+)", msg)
        if m:
            last_rpc = int(m.group(1))
        m = re.search(r"cmdq: wptr (\d+)", msg)
        if m:
            info["doorbells"].append((int(m.group(1)), last_rpc, ts))
        if "msg fn:4097 " in msg and info["init_done"] is None:
            info["init_done"] = ts
        m = re.search(r"gsp: init completed in (\d+)us", msg)
        if m:
            info["init_completed"] = (ts, int(m.group(1)))
        m = re.search(r"fwsec-frts: WPR2 @ ([0-9a-f]+) - ([0-9a-f]+)", msg)
        if m:
            info["wpr2"] = (int(m.group(1), 16), int(m.group(2), 16))
        m = re.search(r"(gsp|sec2\(gsp\)): mbox ([0-9a-f]+) ([0-9a-f]+)", msg)
        if m:
            info["mbox"].setdefault(m.group(1), []).append(
                (int(m.group(2), 16), int(m.group(3), 16)))
        m = re.search(r"gsp: seq (.*)$", body)
        if m and not m.group(1).startswith("0"):
            s = m.group(1).split()
            if s[0] in ("wr32", "mask", "poll", "save"):
                a = int(s[1], 16)
                v = int(s[2], 16) if s[0] in ("wr32", "mask") else None
                if s[0] == "save":
                    a = int(s[1], 16)
                info["seq"].append((s[0], a, v))
            elif s[0] == "usec":
                info["seq"].append(("usec", None, None))
            elif s[0] == "core":
                info["seq"].append(("core_" + "_".join(s[1:]), None, None))
    return info


# ---------------------------------------------------------------- phases

TAIL_DOORBELLS = 3   # cmdq doorbells kept after postinit

def assign_phases(acc, dinfo):
    """Tag each access with phase/step using register-event state machine."""
    phase, step = "pre", "probe"
    seq_ops = dinfo.get("seq", []) if dinfo else []
    seq_i = 0
    postinit_done_at = None
    tail_doorbells = 0
    for k, a in enumerate(acc):
        off, op, val = a["off"], a["op"], a["val"]
        rel = off - GSP if GSP <= off < GSP + 0x2000 else None
        srel = off - SEC2 if SEC2 <= off < SEC2 + 0x2000 else None
        if phase == "pre":
            if rel is not None and rel not in (0xc00,):
                phase, step = "fwsec_frts", "acquire"
            elif rel == 0xc00:
                step = "cmdq_doorbell"
            else:
                step = "probe"
        if phase == "fwsec_frts":
            if rel == 0x048 and step == "acquire":
                step = "reset"
            elif rel == 0x624 and op == "R" and step == "reset":
                step = "load"
            elif rel == 0x040 and op == "W" and step == "load":
                step = "boot"
            elif off == 0x001438:
                step = "verify"
            elif step == "verify" and rel == 0x3c0:
                phase, step = "gsp_riscv_reset", "reset"
        if phase == "gsp_riscv_reset":
            if a.get("dma"):
                step = "libos_args"
            if srel is not None:
                phase, step = "booter_load", "acquire"
        if phase == "booter_load":
            # gm200_flcn_fw_signature: nvkm_falcon_enable + read .debug 0x408
            # (bit20 = debug-fused, picks the debug signature) before reset.
            if srel == 0x3c0 and step == "acquire":
                step = "sig_select"
            elif srel == 0x048 and step == "sig_select":
                step = "reset"
            elif srel == 0x624 and op == "R" and step == "reset":
                step = "load"
            elif srel == 0x040 and op == "W" and step == "load":
                step = "boot"
            elif rel == 0x080 and op == "W":
                phase, step = "gsp_handshake", "app_version"
        if phase == "gsp_handshake":
            if rel == 0x1240:
                step = "riscv_active"
            elif rel is not None and rel != 0x080 and acc[k - 1]["off"] == GSP + 0x1240:
                phase, step = "cpu_sequencer", None
        if phase == "cpu_sequencer":
            # dmesg-guided: advance when this access is the first one of the
            # next logged sequencer op.
            if seq_i < len(seq_ops):
                kind, sa, sv = seq_ops[seq_i]
                first = None
                if kind == "wr32":
                    first = ("W", sa, sv)
                elif kind in ("poll", "mask", "save"):
                    first = ("R", sa, None)
                elif kind == "core_reset":       # nvkm_falcon_reset: disable first
                    first = ("R", GSP + 0x048, None)
                elif kind == "core_resume":      # tu102_gsp_reset = reset_eng 0x3c0
                    first = ("R", GSP + 0x3c0, None)
                elif kind in ("core_start", "core_wait_halt"):
                    first = ("R", GSP + 0x100, None)
                if kind == "usec":
                    seq_i += 1
                elif first and op == first[0] and off == first[1] and \
                        (first[2] is None or val == first[2]):
                    step = "seq_" + kind
                    seq_i += 1
            if step is None:
                step = "seq"
            if rel == 0xc00 and op == "W":
                phase, step = "postinit", "rpc_doorbell"
        if phase == "postinit":
            if off == GSP + 0x004 and op == "W" and val == 0x40 and postinit_done_at is None:
                step = "intr_allow"
                postinit_done_at = k
            elif postinit_done_at is not None:
                phase, step = "post_tail", "doorbell"
            elif rel == 0xc00:
                step = "rpc_doorbell"
            elif off >= 0xb81000 and off < 0xb82000 or rel in (0x008, 0x12b4):
                step = "intr"
        if phase == "post_tail":
            if tail_doorbells >= TAIL_DOORBELLS:
                a["phase"] = "end"
                continue
            if rel == 0xc00 and op == "W":
                tail_doorbells += 1
            step = "rpc_doorbell" if rel == 0xc00 else "intr"
        a["phase"], a["step"] = phase, step
    return acc


# ---------------------------------------------------------------- masking

def mark_mailbox_pairs(acc):
    """A W MAILBOX0 immediately followed by W MAILBOX1 on the same falcon is a
    64-bit DMA address hand-off (GSP: libos args, tu102.c oneinit tail and
    r535 gsp.c CORE_RESUME; SEC2: WPR meta, tu102_gsp_booter_load).  The
    no-op mask() write-backs during reset (gm200.c wait_mem_scrubbing) are
    single writes and stay unmasked."""
    for k in range(len(acc) - 1):
        a, b = acc[k], acc[k + 1]
        for base, kind in ((GSP, "libos"), (SEC2, "wpr_meta")):
            if a["op"] == "W" and b["op"] == "W" and a["off"] == base + 0x040 \
                    and b["off"] == base + 0x044:
                a["dma"], b["dma"] = kind + "_lo", kind + "_hi"


def mask_value(a):
    """Return replacement string for DMA/sysmem-address values, or None."""
    if a["op"] != "W":
        return None
    if a.get("dma"):
        return "<dma:%s>" % a["dma"]
    if a["off"] in (GSP + 0x054, SEC2 + 0x054):
        return "<dma:inst_bind>"
    return None


# flcn_bl_dmem_desc_v2 (include/nvfw/flcn.h): words 9-10 code_dma_base,
# 16-17 data_dma_base.  Only the FWSEC bootloader descriptor carries them.
FWSEC_DESC_DMA_WORDS = (9, 10, 16, 17)


def hx(v):
    return "0x%08x" % v


# ---------------------------------------------------------------- collapse

def collapse(acc):
    recs = []
    pio = None       # (falcon_base, port) in progress
    ctl = {}         # falcon_base -> {"imem": start, "dmem": start, "imem_sec": bool}
    k = 0
    n = len(acc)

    def flush():
        nonlocal pio
        if pio is None:
            return
        words = pio["words"]
        blob = b"".join(struct.pack("<I", w) for w in words)
        masked = list(words)
        mw = []
        if pio["port"] == "dmem" and pio["phase"] == "fwsec_frts" and len(words) == 21:
            for wi in FWSEC_DESC_DMA_WORDS:
                masked[wi] = 0
            mw = list(FWSEC_DESC_DMA_WORDS)
        mblob = b"".join(struct.pack("<I", w) for w in masked)
        r = pio["rec"]
        r.update({"port": pio["port"], "start": hx(pio["start"]),
                  "tag": hx(pio["tags"][0]) if pio["tags"] else None,
                  "bytes": 4 * len(words),
                  "sha256": hashlib.sha256(blob).hexdigest(),
                  "sha256_masked": hashlib.sha256(mblob).hexdigest(),
                  "masked_words": mw})
        notes = []
        if pio["tags"]:
            notes.append("tags %s..%s (%d)" % (hx(pio["tags"][0]), hx(pio["tags"][-1]),
                                               len(pio["tags"])))
        if pio.get("secure"):
            notes.append("secure")
        if len(words) <= 32:
            notes.append("words " + " ".join("%08x" % w for w in masked))
        r["note"] = "; ".join(notes)
        r["val"] = "<pio>"
        recs.append(r)
        pio = None

    while k < n:
        a = acc[k]
        off = a["off"]
        base = GSP if GSP <= off < GSP + 0x1000 else SEC2 if SEC2 <= off < SEC2 + 0x1000 else None
        rel = off - base if base is not None else None
        # PIO data / tag accumulation
        if base is not None and a["op"] == "W" and rel in (0x184, 0x188, 0x1c4):
            port = "dmem" if rel == 0x1c4 else "imem"
            if pio is not None and (pio["base"] != base or pio["port"] != port):
                flush()
            if pio is None:
                c = ctl.get(base, {})
                pio = {"base": base, "port": port, "words": [], "tags": [],
                       "start": c.get(port, 0), "secure": c.get(port + "_sec", False),
                       "phase": a["phase"],
                       "rec": base_rec(a, "PIO", off if rel != 0x188 else base + 0x184)}
            if rel == 0x188:
                pio["tags"].append(a["val"])
            else:
                pio["words"].append(a["val"])
            k += 1
            continue
        flush()
        if base is not None and a["op"] == "W" and rel in (0x180, 0x1c0):
            port = "imem" if rel == 0x180 else "dmem"
            ctl.setdefault(base, {})[port] = a["val"] & 0xfffc
            ctl[base][port + "_sec"] = bool(a["val"] & (1 << 28))
        if a["op"] == "R":
            j = k
            while j + 1 < n and acc[j + 1]["op"] == "R" and acc[j + 1]["off"] == off \
                    and acc[j + 1]["phase"] == a["phase"]:
                j += 1
            if j > k:
                r = base_rec(a, "POLL", off)
                r["val"] = hx(acc[j]["val"])
                r["count"] = j - k + 1
                r["first"] = hx(a["val"])
                r["final"] = hx(acc[j]["val"])
                r["note"] = poll_note(a, acc[j]["val"], acc[j]["ts"] - a["ts"])
                recs.append(r)
                k = j + 1
                continue
        r = base_rec(a, a["op"], off)
        mv = mask_value(a)
        r["val"] = mv if mv else hx(a["val"])
        r["note"] = access_note(a)
        recs.append(r)
        k += 1
    flush()
    for i, r in enumerate(recs):
        r["i"] = i
    return recs


def base_rec(a, op, off):
    blk, name = classify(off)
    return {"i": 0, "ts": round(a["ts"], 6), "phase": a["phase"], "step": a.get("step"),
            "op": op, "reg": "0x%06x" % off, "val": None, "block": blk, "name": name,
            "note": ""}


def poll_note(a, final, dt):
    off = a["off"]
    rel = (off - GSP) if GSP <= off < GSP + 0x1000 else (off - SEC2) if SEC2 <= off < SEC2 + 0x1000 else None
    s = "%.3f ms" % (dt * 1e3)
    if rel == 0x100:
        s += "; wait CPUCTL bit4 (halted)"
    elif rel == 0x10c:
        s += "; wait DMACTL bits 1-2 clear (scrub done)"
    elif off == 0x1180f8:
        s += "; wait bit26 (0x04000000)"
    elif rel == 0x040 and a["phase"] == "cpu_sequencer":
        s += "; seq poll mask 0x80000000"
    return s


def access_note(a):
    off, val, op = a["off"], a["val"], a["op"]
    if off in (0x1fa824, 0x1fa828) and op == "R":
        return "addr 0x%x" % ((val >> 4) << 12)
    if off == 0x001438 and op == "R":
        return "FWSEC-FRTS error 0x%04x" % (val >> 16)
    if off == 0x625f04 and op == "R":
        return "bit3 %s; base field 0x%x" % ("set" if val & 8 else "clear", (val & 0xffffff00) << 8)
    if off == 0x100ce0 and op == "R":
        size = ((val & 0x3f0) >> 4) << ((val & 0xf) + 20)
        if val & 0x40000000:
            size = size // 16 * 15
        return "FB 0x%x (%d MiB), fb/gp102.c gp102_fb_vidmem_size" % (size, size >> 20)
    if (off - GSP) in (0x180,) or (off - SEC2) in (0x180,):
        return "offset 0x%x autoinc %d secure %d" % (val & 0xfffc, (val >> 24) & 1, (val >> 28) & 1)
    if (off - GSP) == 0x1c0 or (off - SEC2) == 0x1c0:
        return "offset 0x%x autoinc-wr %d" % (val & 0xfffc, (val >> 24) & 1)
    if (off - GSP) == 0x100 or (off - SEC2) == 0x100 or (off - SEC2) == 0x130:
        return "STARTCPU" if op == "W" and val & 2 else ""
    return ""


# ---------------------------------------------------------------- report

def summarize(recs):
    per = {}
    for r in recs:
        p = per.setdefault(r["phase"], {"records": 0, "first_ts": r["ts"], "last_ts": r["ts"]})
        p["records"] += 1
        p["last_ts"] = r["ts"]
    return per


def write_jsonl(recs, path):
    keys = ["i", "ts", "phase", "step", "op", "reg", "val", "block", "name", "note"]
    with open(path, "w") as f:
        for r in recs:
            o = {k: r[k] for k in keys}
            if r["op"] == "POLL":
                for k in ("count", "first", "final"):
                    o[k] = r[k]
            if r["op"] == "PIO":
                for k in ("port", "start", "tag", "bytes", "sha256", "sha256_masked",
                          "masked_words"):
                    o[k] = r[k]
            f.write(json.dumps(o) + "\n")


def build(trace, dmesg=None, bar0=BAR0_DEFAULT, raw_counts=None):
    acc, stats = parse_trace(trace, bar0)
    dinfo = dmesg_info(parse_dmesg(dmesg)) if dmesg else {}
    kept = []
    for a in acc:
        blk, name = classify(a["off"])
        if blk is None or blk == "pramin":
            stats["excluded"][name] = stats["excluded"].get(name, 0) + 1
            continue
        kept.append(a)
    mark_mailbox_pairs(kept)
    assign_phases(kept, dinfo)
    kept = [a for a in kept if a.get("phase") != "end"]
    # Beyond postinit only the GSP falcon block stays in scope.
    kept = [a for a in kept if a["phase"] != "post_tail"
            or classify(a["off"])[0] in ("gsp", "gsp_riscv")]
    recs = collapse(kept)
    # doorbell annotations from dmesg (cmdq wptr N <- rpc fn)
    dbs = dinfo.get("doorbells", []) if dinfo else []
    di = 0
    for r in recs:
        if r["reg"] == "0x110c00" and r["op"] == "W":
            if di < len(dbs):
                wp, fn, _ = dbs[di]
                r["note"] = "cmdq doorbell, wptr %d, rpc fn %s" % (wp, fn)
            di += 1
    return recs, stats, dinfo


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--dmesg")
    ap.add_argument("--bar0", type=lambda s: int(s, 0), default=BAR0_DEFAULT)
    ap.add_argument("--jsonl", required=True)
    ap.add_argument("--stats", help="write a JSON stats/anchor file")
    args = ap.parse_args(argv)
    recs, stats, dinfo = build(args.trace, args.dmesg, args.bar0)
    write_jsonl(recs, args.jsonl)
    per = summarize(recs)
    out = {"stats": stats, "phases": per}
    if dinfo:
        out["dmesg"] = {k: dinfo[k] for k in ("anchors", "init_done", "init_completed",
                                              "wpr2", "mbox")}
        out["dmesg"]["seq_ops"] = len(dinfo["seq"])
        out["dmesg"]["doorbells"] = dinfo["doorbells"][:16]
        # trace-minus-dmesg clock offset at the two boot anchors
        offs = {}
        boots = {"fwsec_boot": ("fwsec_frts", "0x110100"), "booter_boot": ("booter_load", "0x840100")}
        for name, (ph, reg) in boots.items():
            t = next((r["ts"] for r in recs if r["phase"] == ph and r["reg"] == reg
                      and r["op"] == "W"), None)
            if t is not None and name in dinfo["anchors"]:
                offs[name] = round(t - dinfo["anchors"][name], 6)
        out["clock_offset_trace_minus_dmesg"] = offs
    if args.stats:
        with open(args.stats, "w") as f:
            json.dump(out, f, indent=1)
    for ph, p in per.items():
        print("%-16s %5d records  %.6f .. %.6f" % (ph, p["records"], p["first_ts"], p["last_ts"]))
    print("unknown(rep movs) %d, other-BAR r/w %d, excluded %s" % (
        stats["unknown"], stats["other_bar_rw"], stats["excluded"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

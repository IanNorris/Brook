#!/usr/bin/env python3
"""Trace-replay diff for the TU104 GSP-RM boot (M0).

Compares Brook's planned register sequence, as recorded by the fake register
file of test_nv_gsp_boot (raw JSONL: op/off/val/phase/step per access), with
the golden reference decoded from the 25 Nov Linux mmiotrace by
nv_gsp_mmiotrace.py. Brook's raw log goes through the *same* normalisation
(mailbox-pair DMA masking, poll collapsing, IMEM/DMEM PIO hashing with the
FWSEC descriptor DMA words masked), then each phase is aligned with
difflib on these keys:

    R / W   (op, reg, val)              val "<dma:...>" when masked
    POLL    (POLL, reg, final value)    counts and first values ignored
    PIO     (PIO, reg, port, start, tag, bytes, sha256_masked)

Subcommands (stdlib only, run with python3 -I):
    extract-seq --dmesg dmesg-final.txt --out seq.bin
        RUN_CPU_SEQUENCER payload (fn 4098) from nouveau's gsp=trace hex dump
    diff --ref gsp-reference-sequence.jsonl --brook brook-raw.jsonl [--md out.md]
"""
import argparse
import difflib
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PHASES = ["pre", "fwsec_frts", "gsp_riscv_reset", "booter_load", "gsp_handshake",
          "cpu_sequencer", "init_done_wait", "postinit", "post_tail"]


def load_decoder():
    spec = importlib.util.spec_from_file_location("nv_gsp_mmiotrace",
                                                  os.path.join(HERE, "nv_gsp_mmiotrace.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ------------------------------------------------------------ extract-seq

MSG_HDR = re.compile(r"gsp:msg fn:(\d+) len:0x([0-9a-f]+)/0x([0-9a-f]+)")
MSG_HEX = re.compile(r"\]\s+msg: ([0-9a-f]{8}): ((?:[0-9a-f]{2} ){1,16})")


def extract_seq(dmesg_path, fn=4098):
    lines = open(dmesg_path, "r", errors="replace").read().splitlines()
    for i, line in enumerate(lines):
        m = MSG_HDR.search(line)
        if not m or int(m.group(1)) != fn:
            continue
        size = int(m.group(3), 16)
        data = bytearray()
        for nxt in lines[i + 1:]:
            h = MSG_HEX.search(nxt)
            if not h:
                break
            off = int(h.group(1), 16)
            if off != len(data):
                raise ValueError("hex dump gap at 0x%x" % off)
            data += bytes(int(b, 16) for b in h.group(2).split())
        if len(data) < size:
            raise ValueError("hex dump short: %d < %d" % (len(data), size))
        return bytes(data[:size])
    raise ValueError("no fn %d message in %s" % (fn, dmesg_path))


# ------------------------------------------------------------------- diff

def load_jsonl(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def normalise_brook(dec, raw):
    acc = []
    for i, r in enumerate(raw):
        acc.append({"op": r["op"], "ts": i * 1e-6, "off": r["off"], "val": r["val"], "w": 4,
                    "phase": r["phase"], "step": r["step"]})
    dec.mark_mailbox_pairs(acc)
    return dec.collapse(acc)


def key(r):
    if r["op"] == "POLL":
        return ("POLL", r["reg"], r["final"])
    if r["op"] == "PIO":
        return ("PIO", r["reg"], r["port"], r["start"], r["tag"], r["bytes"], r["sha256_masked"])
    return (r["op"], r["reg"], r["val"])


def fmt(r):
    if r["op"] == "PIO":
        return "PIO %s %s start %s tag %s %d B sha %s" % (r["reg"], r["port"], r["start"], r["tag"],
                                                          r["bytes"], r["sha256_masked"][:12])
    if r["op"] == "POLL":
        return "POLL %s %s -> %s (x%d)" % (r["reg"], r["name"], r["final"], r["count"])
    return "%s %s %s = %s" % (r["op"], r["reg"], r["name"], r["val"])


def diff(ref, brook):
    out = {"phases": [], "totals": {"equal": 0, "ref_only": 0, "brook_only": 0, "changed": 0}}
    for ph in PHASES:
        a = [r for r in ref if r["phase"] == ph]
        b = [r for r in brook if r["phase"] == ph]
        if not a and not b:
            continue
        sm = difflib.SequenceMatcher(None, [key(r) for r in a], [key(r) for r in b], autojunk=False)
        p = {"phase": ph, "ref": len(a), "brook": len(b), "equal": 0, "ops": []}
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                p["equal"] += i2 - i1
                continue
            p["ops"].append({"tag": tag, "ref": a[i1:i2], "brook": b[j1:j2]})
            if tag == "delete":
                out["totals"]["ref_only"] += i2 - i1
            elif tag == "insert":
                out["totals"]["brook_only"] += j2 - j1
            else:
                out["totals"]["changed"] += max(i2 - i1, j2 - j1)
        out["totals"]["equal"] += p["equal"]
        out["phases"].append(p)
    return out


def report_md(d, ref_path, brook_path):
    t = d["totals"]
    lines = ["# GSP boot trace diff: Brook plan vs Linux nouveau (mmiotrace)", "",
             "- reference: `%s`" % ref_path, "- brook: `%s`" % brook_path,
             "- totals: %d equal, %d changed, %d reference-only, %d Brook-only" %
             (t["equal"], t["changed"], t["ref_only"], t["brook_only"]), "",
             "| phase | ref records | brook records | equal |", "|---|---|---|---|"]
    for p in d["phases"]:
        lines.append("| %s | %d | %d | %d |" % (p["phase"], p["ref"], p["brook"], p["equal"]))
    for p in d["phases"]:
        if not p["ops"]:
            continue
        lines += ["", "## %s" % p["phase"], ""]
        for op in p["ops"]:
            lines.append("- **%s**" % op["tag"])
            for r in op["ref"][:12]:
                lines.append("  - ref   [%s] %s" % (r.get("step"), fmt(r)))
            if len(op["ref"]) > 12:
                lines.append("  - ref   ... %d more" % (len(op["ref"]) - 12))
            for r in op["brook"][:12]:
                lines.append("  - brook [%s] %s" % (r.get("step"), fmt(r)))
            if len(op["brook"]) > 12:
                lines.append("  - brook ... %d more" % (len(op["brook"]) - 12))
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract-seq")
    e.add_argument("--dmesg", required=True)
    e.add_argument("--out", required=True)
    d = sub.add_parser("diff")
    d.add_argument("--ref", required=True)
    d.add_argument("--brook", required=True)
    d.add_argument("--md")
    d.add_argument("--json")
    args = ap.parse_args(argv)
    if args.cmd == "extract-seq":
        data = extract_seq(args.dmesg)
        with open(args.out, "wb") as f:
            f.write(data)
        print("wrote %d bytes (bufferSizeDWord 0x%x cmdIndex %d)" %
              (len(data), int.from_bytes(data[0:4], "little"), int.from_bytes(data[4:8], "little")))
        return 0
    dec = load_decoder()
    ref = load_jsonl(args.ref)
    brook = normalise_brook(dec, load_jsonl(args.brook))
    res = diff(ref, brook)
    md = report_md(res, args.ref, args.brook)
    if args.md:
        with open(args.md, "w") as f:
            f.write(md)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(res, f, indent=1)
    t = res["totals"]
    for p in res["phases"]:
        print("%-16s ref %4d brook %4d equal %4d" % (p["phase"], p["ref"], p["brook"], p["equal"]))
    print("total: %d equal, %d changed, %d ref-only, %d brook-only" %
          (t["equal"], t["changed"], t["ref_only"], t["brook_only"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

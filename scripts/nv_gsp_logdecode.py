#!/usr/bin/env python3
"""Host decoder for GSP-RM libos log buffers (LOGINIT / LOGINTR / LOGRM).

Provenance: clean-room Python written from the MIT-licensed open-gpu-kernel-modules
sources at tag 570.144 (no code copied):
  src/common/uproc/os/libos-v3.1.0/lib/liblogdecode.c   record layout, backward
      scan from the put pointer, wrap handling, printf subset, special types
  src/common/uproc/os/common/include/libos_log.h        libosLogMetadata{,_extended}
  src/common/uproc/os/libos-v3.1.0/lib/libelf.c         VA -> file mapping (PHDR, then SHDR)
  src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c            buffer names, sizes, per-task
      ".fwlogging_init" / ".fwlogging_rm" sections; LOGINTR shares the rm ELF
  src/nvidia/generated/g_kernel_gsp_nvoc.h              GSP_LOGGING_SECTION_NAME ".fwlogging"
  src/nvidia/arch/nvalloc/unix/src/osinit.c, kernel-open/common/inc/nv-firmware.h
      the logging ELF is a separate file, gsp_log_<chip>.bin (gsp_log_tu10x.bin for
      TU10x). NVIDIA does not ship it: neither linux-firmware nor the 570.144 .run
      package contains it, and gsp_tu10x.bin / gsp-570.144.bin has no .fwlogging.
  src/nvidia/inc/libraries/utils/nvprintf_level.h       LEVEL_* and RM_GSP_LOG_SPECIAL_*

Buffer layout (u64 words, little endian): word 0 = put = number of words ever
written; data words follow, logical word k lives at 1 + k % (size/8 - 1). Each
record, in write order, is
    [arg0 .. argN-1][metadata VA][timestamp]
(N = metadata.argumentCount). The capture file is the buffer image from offset 0
(the bridge's `nv-gsp log <which> <off>` dumps 1536 B chunks; concatenate them from
offset 0). Words the capture does not cover are treated as missing.

With --elf (gsp_log_*.bin container, its .fwlogging ELF, or a per-task ELF) the
records are decoded exactly as libos does, backwards from put. Without it, record
boundaries are inferred forwards: a record ends at a (metadata-like, timestamp-like)
pair with timestamps non-decreasing, and once a metadata pointer's argument count
is known it is reused. --image <gsp-*.bin or its .fwimage ELF> (public) resolves
arguments that point at strings in the task rodata (the usual %s case), in both
modes; format strings themselves exist only in the logging ELF.
Stdlib only:  python3 -I nv_gsp_logdecode.py [--elf gsp_log_tu10x.bin] [--image gsp-570.144.bin] log-rm.bin
"""
import argparse
import os
import struct
import sys
from collections import Counter, OrderedDict

LEVELS = {0: "SILENT", 1: "INFO", 2: "NOTICE", 3: "WARNING", 4: "ERROR", 5: "HW_ERROR", 6: "FATAL"}
SPECIAL_ASSERT, SPECIAL_ASSERT_OK, SPECIAL_CHECK, SPECIAL_CHECK_OK = 1, 2, 3, 4
MAX_ARGS = 20
EXTENDED_FILENAME = 0x8000000000000000
SECTION_FOR = {"init": ".fwlogging_init", "rm": ".fwlogging_rm", "intr": ".fwlogging_rm"}


class Elf:
    """Minimal ELF64 LE reader: sections by name, VA mapping via PT_LOAD then SHDR."""

    def __init__(self, data):
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise ValueError("not an ELF64 little-endian image")
        self.data = data
        (phoff, shoff) = struct.unpack_from("<QQ", data, 0x20)
        (phentsize, phnum, shentsize, shnum, shstrndx) = struct.unpack_from("<HHHHH", data, 0x36)
        self.phdrs = []
        for i in range(phnum):
            p_type, _flags, off, vaddr, _pa, filesz, _memsz, _al = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
            if p_type == 1 and filesz:
                self.phdrs.append((vaddr, off, filesz))
        raw = []
        for i in range(shnum):
            raw.append(struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize))
        strtab = raw[shstrndx] if shstrndx < len(raw) else None
        self.sections = OrderedDict()
        for (name, typ, _fl, addr, off, size, _l, _i, _a, _e) in raw:
            nm = ""
            if strtab:
                s = strtab[4] + name
                nm = data[s:data.index(b"\0", s)].decode("latin-1")
            self.sections[nm] = (typ, addr, off, size)

    def section_bytes(self, name):
        s = self.sections.get(name)
        if not s or s[0] == 8:  # NOBITS
            return None
        return self.data[s[2]:s[2] + s[3]]

    def _file_off(self, va, size):
        for vaddr, off, filesz in self.phdrs:
            if vaddr <= va and va + size <= vaddr + filesz:
                return off + va - vaddr
        for typ, addr, off, ssize in self.sections.values():
            if typ != 8 and addr and addr <= va and va + size <= addr + ssize:
                return off + va - addr
        return None

    def read(self, va, size):
        o = self._file_off(va, size)
        return None if o is None else self.data[o:o + size]

    def string(self, va):
        o = self._file_off(va, 1)
        if o is None:
            return None
        end = self.data.find(b"\0", o)
        return self.data[o:end if end >= 0 else len(self.data)].decode("utf-8", "replace")


def load_meta_elf(path, name, section=None):
    """Unwrap gsp_log_*.bin -> .fwlogging -> (.fwIsContainerElf) -> .fwlogging_<task>."""
    elf = Elf(open(path, "rb").read())
    inner = elf.section_bytes(".fwlogging")
    if inner is not None:
        elf = Elf(inner)
    if ".fwIsContainerElf" in elf.sections:
        sec = section or SECTION_FOR.get(name or "", ".fwlogging_rm")
        task = elf.section_bytes(sec)
        if task is None:
            raise ValueError("container logging ELF has no section %s (has: %s)" % (sec, ", ".join(elf.sections)))
        elf = Elf(task)
    return elf


class Meta:
    __slots__ = ("va", "filename", "fmt", "line", "argc", "level", "special", "remote")

    def __init__(self, va, filename, fmt, line, argc, level, special, remote=None):
        self.va, self.filename, self.fmt, self.line = va, filename, fmt, line
        self.argc, self.level, self.special, self.remote = argc, level, special, remote


def load_image(path):
    elf = Elf(open(path, "rb").read())
    inner = elf.section_bytes(".fwimage")
    return Elf(inner) if inner is not None else elf


def image_string(image, v):
    if image is None or v < 0x1000 or v >= (1 << 48):
        return None
    s = image.string(v)
    if s is None or len(s) < 2 or len(s) > 200 or not all(32 <= ord(c) < 127 for c in s):
        return None
    return s


def lookup_meta(elf, va, cache):
    if va in cache:
        return cache[va]
    m = None
    raw = elf.read(va, 24) if elf else None
    if raw:
        fn, fmt, line, argc, level, special = struct.unpack_from("<QQIBBB", raw)
        remote = None
        if fn == EXTENDED_FILENAME and elf.string(fn) is None:
            raw = elf.read(va, 40)
            if raw:
                _ver, sect = struct.unpack_from("<QQ", raw)
                fn, fmt, line, argc, level, special = struct.unpack_from("<QQIBBB", raw, 16)
                remote = elf.string(sect) if sect else None
        fstr = elf.string(fmt)
        if fstr is not None and argc <= MAX_ARGS:
            name = elf.string(fn) or "unknown"
            m = Meta(va, name.rsplit("/", 1)[-1], fstr, line, argc, level, special, remote)
    cache[va] = m
    return m


def s64(v):
    return v - (1 << 64) if v & (1 << 63) else v


def libos_format(fmt, args, elf, image=None):
    """printf subset as liblogdecode: flags -+ #0, width/precision (incl. *),
    length hh h l ll z j t L q (only hh/h truncate), conversions d i u x X o c s p a C %."""
    out, ai, i, n = [], 0, 0, len(fmt)

    def nxt():
        nonlocal ai
        v = args[ai] if ai < len(args) else 0
        ai += 1
        return v

    while i < n:
        c = fmt[i]
        if c != "%":
            out.append(c)
            i += 1
            continue
        i += 1
        if i < n and fmt[i] == "%":
            out.append("%")
            i += 1
            continue
        flags = ""
        while i < n and fmt[i] in "-+ #0'":
            flags += fmt[i]
            i += 1
        width = ""
        if i < n and fmt[i] == "*":
            width = str(s64(nxt()))
            i += 1
        while i < n and fmt[i].isdigit():
            width += fmt[i]
            i += 1
        prec = None
        if i < n and fmt[i] == ".":
            i += 1
            prec = ""
            if i < n and fmt[i] == "*":
                prec = str(max(0, s64(nxt())))
                i += 1
            while i < n and fmt[i].isdigit():
                prec += fmt[i]
                i += 1
        length = ""
        while i < n and fmt[i] in "hlzjtLq":
            length += fmt[i]
            i += 1
        if i >= n:
            break
        t = fmt[i]
        i += 1
        spec = "%" + flags.replace("'", "") + width + ("" if prec is None else "." + (prec or "0"))
        v = nxt() if t != "n" else 0
        if length == "hh":
            v &= 0xFF
        elif length == "h":
            v &= 0xFFFF
        if t in "di":
            if length in ("hh", "h"):
                bits = 8 if length == "hh" else 16
                v = v - (1 << bits) if v & (1 << (bits - 1)) else v
            else:
                v = s64(v)
            out.append((spec + "d") % v)
        elif t in "uxXo":
            out.append((spec + {"u": "d"}.get(t, t)) % v)
        elif t in "pa":
            out.append("0x%x" % v)
        elif t in "cC":
            out.append(chr(v & 0xFF if t == "c" else v & 0x10FFFF))
        elif t == "s":
            s = elf.string(v) if elf else None
            if s is None:
                s = image_string(image, v)
            if s is None:
                s = "(bad-pointer 0x%x)" % v
            out.append((spec.replace("0", "") + "s") % s if (width or prec) else s)
        elif t == "n":
            pass
        else:
            out.append("%" + t)
    return "".join(out)


def nvstatus(v):
    return "NV_STATUS 0x%08X" % (v & 0xFFFFFFFF)


class Ring:
    def __init__(self, data, buffer_size):
        nwords = len(data) // 8
        self.words = struct.unpack_from("<%dQ" % nwords, data)
        self.put = self.words[0]
        self.entries = buffer_size // 8 - 1
        self.start = max(0, self.put - self.entries)
        self.captured = nwords

    def w(self, k):
        p = 1 + k % self.entries
        return self.words[p] if p < self.captured else None


def decode_elf(ring, elf):
    """Exact libos order: walk back from put: timestamp, metadata, args."""
    recs, cache, i = [], {}, ring.put
    while i > ring.start:
        if i - 2 < ring.start:
            break
        ts, mva = ring.w(i - 1), ring.w(i - 2)
        if ts is None or mva is None:
            return list(reversed(recs)), i, "capture does not cover logical word %d" % (i - 1)
        m = lookup_meta(elf, mva, cache)
        if m is None:
            return list(reversed(recs)), i, "bad metadata 0x%x at logical word %d" % (mva, i - 2)
        if i - 2 - m.argc < ring.start:
            return list(reversed(recs)), i, "buffer wrapped inside a record"
        args = [ring.w(k) for k in range(i - 2 - m.argc, i - 2)]
        if any(a is None for a in args):
            return list(reversed(recs)), i, "capture does not cover args"
        recs.append((i - 2 - m.argc, ts, mva, args, m))
        i -= 2 + m.argc
    return list(reversed(recs)), i, None


def decode_raw(ring, lo, hi, argc_known, meta_ok=None):
    """Forward inference over logical words [lo, hi). Returns records and leftovers."""
    words = [ring.w(k) for k in range(lo, hi)]
    n = len(words)
    if n < 2 or words[-1] is None or words[-2] is None:
        return [], []
    final_ts, final_meta = words[-1], words[-2]
    span = 64 << 20
    if meta_ok is None:
        def meta_ok(v):
            return v is not None and v & 7 == 0 and 0 < v < (1 << 40) and abs(v - final_meta) < span
    recs, orphans, p, last_ts = [], [], 0, 0
    while p < n:
        found = None
        for q in range(p, min(n - 1, p + MAX_ARGS + 1)):
            mv, tv = words[q], words[q + 1]
            if not meta_ok(mv) or tv is None or not (last_ts <= tv <= final_ts) or meta_ok(tv):
                continue
            if mv in argc_known and argc_known[mv] != q - p:
                continue
            found = q
            break
        if found is None:
            orphans.append((lo + p, words[p]))
            p += 1
            continue
        args = words[p:found]
        argc_known.setdefault(words[found], len(args))
        recs.append((lo + p, words[found + 1], words[found], args, None))
        last_ts = words[found + 1]
        p = found + 2
    return recs, orphans


def fmt_ts(ts, t0):
    return "%12.6f +%10.6f" % (ts / 1e9, (ts - t0) / 1e9)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture", help="buffer image starting at offset 0 (word 0 = put)")
    ap.add_argument("--elf", help="gsp_log_<chip>.bin, its .fwlogging ELF, or a per-task logging ELF")
    ap.add_argument("--image", help="gsp-<ver>.bin or its .fwimage ELF, to resolve string arguments")
    ap.add_argument("--name", choices=["init", "rm", "intr"], help="buffer (default: from file name)")
    ap.add_argument("--section", help="override the container section (default .fwlogging_init/.fwlogging_rm)")
    ap.add_argument("--buffer-size", type=lambda s: int(s, 0), default=0x10000,
                    help="log buffer bytes incl. put word (default 0x10000, kernel_gsp.c release size)")
    ap.add_argument("--argc", action="append", default=[], metavar="VA=N",
                    help="raw mode: known argument count for a metadata pointer (repeatable)")
    a = ap.parse_args(argv)

    name = a.name
    if not name:
        base = os.path.basename(a.capture).lower()
        name = next((k for k in ("init", "intr", "rm") if k in base), None)
    data = open(a.capture, "rb").read()
    if len(data) < 8:
        print("empty capture")
        return 1
    ring = Ring(data, a.buffer_size)
    elf = load_meta_elf(a.elf, name, a.section) if a.elf else None
    image = load_image(a.image) if a.image else None

    print("# %s: buffer %s, put %d words (0x%x), ring %d words, captured %d words, valid logical [%d, %d)%s"
          % (os.path.basename(a.capture), name or "?", ring.put, ring.put, ring.entries, ring.captured - 1,
             ring.start, ring.put, ", WRAPPED (%d words lost)" % ring.start if ring.start else ""))
    if ring.put == 0:
        print("# empty: put == 0 (no records written)")
        return 0
    if ring.put + 1 > ring.captured and ring.start == 0:
        print("# warning: capture ends before put; records past word %d are missing" % (ring.captured - 1))

    argc_known = {}
    for item in a.argc:
        k, v = item.split("=")
        argc_known[int(k, 0)] = int(v, 0)

    recs, note, leftover_hi = [], None, None
    if elf:
        recs, stop, note = decode_elf(ring, elf)
        if note:
            leftover_hi = stop
    else:
        hi = min(ring.put, ring.captured - 1) if ring.start == 0 else ring.put
        recs, orphans = decode_raw(ring, ring.start, hi, argc_known)
        if orphans:
            note = "%d word(s) not attributed to any record (first at logical %d)" % (len(orphans), orphans[0][0])
    if note:
        print("# note: " + note)
    if elf and leftover_hi is not None and leftover_hi > ring.start:
        raw_recs, _ = decode_raw(ring, ring.start, leftover_hi, argc_known)
        print("# older region [%d, %d) inferred without metadata: %d records" % (ring.start, leftover_hi, len(raw_recs)))
        recs = raw_recs + recs

    t0 = recs[0][1] if recs else 0
    print("# columns: seconds(ts/1e9) +since-first  [logical word]  " +
          ("LEVEL file:line: message" if elf else "metadata-VA argc: args"))
    for (k, ts, mva, args, m) in recs:
        if m is None:
            m = lookup_meta(elf, mva, {}) if elf else None
        partial = m is not None and len(args) != m.argc
        if m is None or partial:
            shown = []
            for x in args:
                st = image_string(image, x)
                shown.append("0x%x" % x if st is None else '0x%x="%s"' % (x, st))
            extra = " (partial: oldest record cut by ring wrap; %s:%d wants %d args)" % (m.filename, m.line, m.argc) if partial else ""
            print("%s [%4d] meta=0x%x argc=%d: %s%s" % (fmt_ts(ts, t0), k, mva, len(args), " ".join(shown), extra))
            continue
        body_args = list(args)
        prefix = ""
        if m.special in (SPECIAL_ASSERT_OK, SPECIAL_CHECK_OK) and body_args:
            prefix = "%s failed: %s returned from " % ("Assertion" if m.special == SPECIAL_ASSERT_OK else "Check",
                                                       nvstatus(body_args.pop(0)))
            msg = m.fmt
        elif m.special in (SPECIAL_ASSERT, SPECIAL_CHECK):
            prefix = "Assertion failed: " if m.special == SPECIAL_ASSERT else "Check failed: "
            msg = m.fmt
        else:
            msg = libos_format(m.fmt, body_args, elf, image)
        print("%s [%4d] %-8s %s:%d: %s%s%s" % (fmt_ts(ts, t0), k, LEVELS.get(m.level, "L%d" % m.level), m.filename,
                                              m.line, prefix, msg.rstrip("\n"),
                                              " [%s]" % m.remote if m.remote else ""))

    if not elf:
        cnt = Counter(r[2] for r in recs)
        print("# %d records, %d distinct metadata pointers (VA: argc x count)" % (len(recs), len(cnt)))
        for va in sorted(cnt):
            print("#   0x%x: argc %d x %d" % (va, argc_known.get(va, -1), cnt[va]))
    else:
        print("# %d records" % len(recs))
    return 0


if __name__ == "__main__":
    sys.exit(main())

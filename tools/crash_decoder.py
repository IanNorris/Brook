#!/usr/bin/env python3
"""Brook OS Crash Decoder — decodes QR-encoded panic dumps from screenshots."""

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

# ── Base45 codec ────────────────────────────────────────────────────────────
BASE45_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

def base45_decode(s: str) -> bytes:
    """Decode a Base45-encoded string to bytes."""
    table = {c: i for i, c in enumerate(BASE45_CHARSET)}
    buf = []
    i = 0
    while i < len(s):
        if i + 2 < len(s):
            c0, c1, c2 = table[s[i]], table[s[i + 1]], table[s[i + 2]]
            val = c0 + c1 * 45 + c2 * 45 * 45
            buf.append((val >> 8) & 0xFF)
            buf.append(val & 0xFF)
            i += 3
        elif i + 1 < len(s):
            c0, c1 = table[s[i]], table[s[i + 1]]
            val = c0 + c1 * 45
            buf.append(val & 0xFF)
            i += 2
        else:
            raise ValueError(f"Base45: trailing character at position {i}")
    return bytes(buf)


# ── Constants ───────────────────────────────────────────────────────────────
PANIC_MAGIC = 0x2D
PANIC_PAD   = 0xCAFEF00D

PKT_CPU_REGS    = 0xA3000001
PKT_STACK_TRACE = 0xA3000002

GPR_NAMES = [
    "RAX", "RBX", "RCX", "RDX", "RSI", "RDI",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
]
EXTRA_NAMES = ["RIP", "RSP", "RBP", "RFLAGS"]
CR_NAMES    = ["CR0", "CR2", "CR3", "CR4"]
SEG_NAMES   = ["CS", "DS", "SS", "ES", "FS", "GS"]


# ── Data classes ────────────────────────────────────────────────────────────
class PanicHeader:
    SIZE = 8

    def __init__(self, data: bytes):
        if len(data) < self.SIZE:
            raise ValueError("Truncated PanicHeader")
        self.magic, self.version, self.page, self.page_count, self.pad = \
            struct.unpack_from("<BBBBI", data, 0)

    def validate(self):
        if self.magic != PANIC_MAGIC:
            raise ValueError(f"Bad magic: 0x{self.magic:02X} (expected 0x{PANIC_MAGIC:02X})")
        if self.pad != PANIC_PAD:
            raise ValueError(f"Bad pad: 0x{self.pad:08X} (expected 0x{PANIC_PAD:08X})")


class PacketHeader:
    SIZE = 8

    def __init__(self, data: bytes, off: int = 0):
        if len(data) - off < self.SIZE:
            raise ValueError("Truncated PacketHeader")
        self.type, self.size = struct.unpack_from("<II", data, off)


class CPURegs:
    def __init__(self, data: bytes):
        off = 0
        # 14 GPRs
        self.gprs = {}
        for name in GPR_NAMES:
            self.gprs[name] = struct.unpack_from("<Q", data, off)[0]
            off += 8
        # RIP, RSP, RBP, RFLAGS
        self.extra = {}
        for name in EXTRA_NAMES:
            self.extra[name] = struct.unpack_from("<Q", data, off)[0]
            off += 8
        # CR0..CR4
        self.crs = {}
        for name in CR_NAMES:
            self.crs[name] = struct.unpack_from("<Q", data, off)[0]
            off += 8
        # Segment registers
        self.segs = {}
        for name in SEG_NAMES:
            self.segs[name] = struct.unpack_from("<H", data, off)[0]
            off += 2
        # reserved
        self.reserved = struct.unpack_from("<H", data, off)[0]

    @property
    def rip(self):
        return self.extra["RIP"]

    def to_dict(self):
        d = {}
        d.update(self.gprs)
        d.update(self.extra)
        d.update(self.crs)
        d.update({k: v for k, v in self.segs.items()})
        return d


class StackTrace:
    def __init__(self, data: bytes):
        if len(data) < 1:
            raise ValueError("Truncated StackTrace")
        self.depth = data[0]
        self.frames = []
        off = 1
        for _ in range(self.depth):
            if off + 8 > len(data):
                break
            self.frames.append(struct.unpack_from("<Q", data, off)[0])
            off += 8


# ── Symbolication ───────────────────────────────────────────────────────────
class Symbolicator:
    def __init__(self, elf_path: str):
        self.elf_path = elf_path
        self.symbols: list[tuple[int, str]] = []  # sorted (addr, name)
        self._load_symbols()

    def _load_symbols(self):
        for tool in ("llvm-nm", "nm"):
            try:
                r = subprocess.run(
                    [tool, "--defined-only", "-n", self.elf_path],
                    capture_output=True, text=True, timeout=10,
                )
                if r.returncode == 0:
                    self._parse_nm(r.stdout)
                    return
            except FileNotFoundError:
                continue
        print(f"[warn] nm/llvm-nm not found — symbolication disabled", file=sys.stderr)

    def _parse_nm(self, output: str):
        for line in output.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                try:
                    addr = int(parts[0], 16)
                    name = parts[2]
                    self.symbols.append((addr, name))
                except ValueError:
                    pass
        self.symbols.sort()

    def resolve(self, addr: int) -> str | None:
        if not self.symbols:
            return None
        lo, hi = 0, len(self.symbols) - 1
        best = None
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.symbols[mid][0] <= addr:
                best = mid
                lo = mid + 1
            else:
                hi = mid - 1
        if best is not None:
            sym_addr, sym_name = self.symbols[best]
            offset = addr - sym_addr
            if offset < 0x100000:  # reasonable offset limit
                return f"{sym_name}+0x{offset:x}"
        return None

    def addr2line(self, addr: int) -> str | None:
        for tool in ("llvm-addr2line", "addr2line"):
            try:
                r = subprocess.run(
                    [tool, "-e", self.elf_path, f"0x{addr:x}"],
                    capture_output=True, text=True, timeout=5,
                )
                if r.returncode == 0:
                    line = r.stdout.strip()
                    if line and line != "??:0" and line != "??:?":
                        return line
            except FileNotFoundError:
                continue
        return None


# ── QR scanning ─────────────────────────────────────────────────────────────
def scan_qr(image_path: str) -> str:
    try:
        from pyzbar.pyzbar import decode as pyzbar_decode
        from PIL import Image
    except ImportError:
        raise RuntimeError(
            "pyzbar/Pillow not installed. Use --base45 to supply raw text instead.\n"
            "  pip install pyzbar Pillow"
        )
    img = Image.open(image_path)
    results = pyzbar_decode(img)
    if not results:
        raise RuntimeError(f"No QR code found in {image_path}")
    return results[0].data.decode("ascii")


# ── VNC capture ─────────────────────────────────────────────────────────────
def vnc_capture(host_port: str) -> str:
    """Capture a screenshot from VNC and scan for QR codes."""
    try:
        from PIL import Image
    except ImportError:
        raise RuntimeError("Pillow not installed — cannot do VNC capture")

    capture_path = "vnc_crash_capture.png"
    # Try vncdotool first, fall back to a simple PIL-based VNC grab
    try:
        subprocess.run(
            ["vncdotool", "-s", host_port, "capture", capture_path],
            check=True, timeout=15,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        raise RuntimeError(
            f"vncdotool failed. Capture a screenshot manually and pass as argument."
        )
    return scan_qr(capture_path)


# ── Pretty printing ─────────────────────────────────────────────────────────
W = 56  # box width

def _box_top():
    return f"  ┌{'─' * W}┐"

def _box_mid():
    return f"  ├{'─' * W}┤"

def _box_bot():
    return f"  └{'─' * W}┘"

def _box_line(text: str):
    return f"  │ {text:<{W - 2}} │"


def print_report(hdr: PanicHeader, regs: CPURegs | None, trace: StackTrace | None,
                 sym: Symbolicator | None, raw_data: bytes, show_raw: bool):
    bar = "═" * (W + 4)
    print(f"\n  {bar}")
    print(f"  {'BROOK OS CRASH DUMP':^{W + 4}}")
    print(f"  {'Version: 0x%02X  Page: %d/%d' % (hdr.version, hdr.page + 1, hdr.page_count):^{W + 4}}")
    print(f"  {bar}\n")

    if regs:
        print("  CPU Registers:")
        print(_box_top())
        # GPRs — two per line
        names = GPR_NAMES
        for i in range(0, len(names), 2):
            a, b = names[i], names[i + 1] if i + 1 < len(names) else None
            left = f"{a:<6s} 0x{regs.gprs[a]:016X}"
            if b:
                right = f"{b:<6s} 0x{regs.gprs[b]:016X}"
                print(_box_line(f"{left}  {right}"))
            else:
                print(_box_line(left))

        # RIP, RSP, RBP, RFLAGS
        print(_box_line(f"{'RIP':<6s} 0x{regs.extra['RIP']:016X}  {'RSP':<6s} 0x{regs.extra['RSP']:016X}"))
        print(_box_line(f"{'RBP':<6s} 0x{regs.extra['RBP']:016X}  {'RFLAGS':<6s} 0x{regs.extra['RFLAGS']:016X}"))

        # Control registers
        print(_box_mid())
        print(_box_line(f"{'CR0':<6s} 0x{regs.crs['CR0']:016X}  {'CR2':<6s} 0x{regs.crs['CR2']:016X}"))
        print(_box_line(f"{'CR3':<6s} 0x{regs.crs['CR3']:016X}  {'CR4':<6s} 0x{regs.crs['CR4']:016X}"))

        # Segment registers
        seg_parts = [f"{n}=0x{regs.segs[n]:04X}" for n in SEG_NAMES]
        print(_box_line("  ".join(seg_parts)))
        print(_box_bot())

    if trace:
        print(f"\n  Stack Trace ({len(trace.frames)} frames):")
        for i, addr in enumerate(trace.frames):
            sym_str = ""
            if sym:
                name = sym.resolve(addr)
                if name:
                    sym_str = f"  {name}"
                    loc = sym.addr2line(addr)
                    if loc:
                        sym_str += f"  ({loc})"
            if not sym_str and addr > 0x7FFF00000000:
                sym_str = "  (userspace)"
            print(f"  #{i:02d}  0x{addr:016X}{sym_str}")

    if show_raw:
        print(f"\n  Raw hex dump ({len(raw_data)} bytes):")
        for off in range(0, len(raw_data), 16):
            chunk = raw_data[off:off + 16]
            hex_part = " ".join(f"{b:02X}" for b in chunk)
            print(f"  {off:04X}  {hex_part}")

    print(f"\n  {bar}\n")


def build_json(hdr: PanicHeader, regs: CPURegs | None, trace: StackTrace | None,
               sym: Symbolicator | None) -> dict:
    out: dict = {
        "header": {
            "magic": f"0x{hdr.magic:02X}",
            "version": hdr.version,
            "page": hdr.page + 1,
            "page_count": hdr.page_count,
        }
    }
    if regs:
        rd = {}
        for k, v in regs.gprs.items():
            rd[k] = f"0x{v:016X}"
        for k, v in regs.extra.items():
            rd[k] = f"0x{v:016X}"
        for k, v in regs.crs.items():
            rd[k] = f"0x{v:016X}"
        for k, v in regs.segs.items():
            rd[k] = f"0x{v:04X}"
        out["registers"] = rd
    if trace:
        frames = []
        for addr in trace.frames:
            entry: dict = {"address": f"0x{addr:016X}"}
            if sym:
                name = sym.resolve(addr)
                if name:
                    entry["symbol"] = name
            frames.append(entry)
        out["stack_trace"] = frames
    return out


# ── Main ────────────────────────────────────────────────────────────────────
def decode_crash(data: bytes, sym: Symbolicator | None,
                 show_raw: bool, as_json: bool):
    hdr = PanicHeader(data)
    hdr.validate()

    off = PanicHeader.SIZE
    regs = None
    trace = None

    while off + PacketHeader.SIZE <= len(data):
        pkt = PacketHeader(data, off)
        payload_start = off + PacketHeader.SIZE
        payload_end = payload_start + pkt.size

        if payload_end > len(data):
            print(f"[warn] Packet at offset {off} truncated "
                  f"(need {pkt.size}B, have {len(data) - payload_start}B)",
                  file=sys.stderr)
            payload_end = len(data)

        payload = data[payload_start:payload_end]

        if pkt.type == PKT_CPU_REGS:
            regs = CPURegs(payload)
        elif pkt.type == PKT_STACK_TRACE:
            trace = StackTrace(payload)
        else:
            print(f"[warn] Unknown packet type 0x{pkt.type:08X} ({pkt.size}B)",
                  file=sys.stderr)

        off = payload_end

    if as_json:
        print(json.dumps(build_json(hdr, regs, trace, sym), indent=2))
    else:
        print_report(hdr, regs, trace, sym, data, show_raw)


def main():
    ap = argparse.ArgumentParser(
        description="Brook OS crash decoder — decode QR panic dumps")
    ap.add_argument("image", nargs="?", help="Path to screenshot image with QR code")
    ap.add_argument("--base45", metavar="TEXT", help="Decode raw Base45 text directly")
    ap.add_argument("--elf", metavar="PATH", help="Path to BROOK.elf for symbolication")
    ap.add_argument("--vnc", metavar="HOST:PORT", help="Capture from VNC server")
    ap.add_argument("--raw", action="store_true", help="Show raw hex dump")
    ap.add_argument("--json", action="store_true", help="Output as JSON")
    args = ap.parse_args()

    # Obtain Base45 text
    b45_text = None
    if args.base45:
        b45_text = args.base45
    elif args.vnc:
        b45_text = vnc_capture(args.vnc)
    elif args.image:
        b45_text = scan_qr(args.image)
    else:
        ap.error("Provide an image path, --base45 text, or --vnc host:port")

    # Decode
    try:
        raw = base45_decode(b45_text)
    except (KeyError, ValueError) as e:
        print(f"[error] Base45 decode failed: {e}", file=sys.stderr)
        sys.exit(1)

    # Symbolicator
    sym = Symbolicator(args.elf) if args.elf else None

    decode_crash(raw, sym, args.raw, args.json)


if __name__ == "__main__":
    main()

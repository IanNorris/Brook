#!/usr/bin/env python3
"""Brook OS Crash Decoder — decodes QR-encoded panic dumps from screenshots.

Usage:
  crash_decoder.py screenshot.png          # Decode QR from image
  crash_decoder.py --vnc localhost:5943    # Capture from VNC + decode
  crash_decoder.py --base45 "1A2B..."      # Decode raw Base45 text

Automatically finds BROOK.elf for symbolication if not specified.
"""

import argparse
import json
import os
import socket
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

PKT_CPU_REGS       = 0xA3000001
PKT_STACK_TRACE    = 0xA3000002
PKT_EXCEPTION_INFO = 0xA3000003
PKT_PROCESS_LIST   = 0xA3000004

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


EXCEPTION_NAMES = {
    0: "Divide Error", 1: "Debug", 2: "NMI", 3: "Breakpoint",
    4: "Overflow", 5: "BOUND", 6: "#UD Invalid Opcode",
    7: "Device Not Available", 8: "Double Fault", 10: "#TS Invalid TSS",
    11: "#NP Segment Not Present", 12: "#SS Stack Segment",
    13: "#GP General Protection", 14: "#PF Page Fault",
    16: "#MF x87 FP", 17: "#AC Alignment Check",
    18: "#MC Machine Check", 19: "#XM SIMD FP",
}

PROCESS_STATE_NAMES = {
    0: "Ready", 1: "Running", 2: "Blocked", 3: "Stopped", 4: "Terminated",
}


class ExceptionInfo:
    def __init__(self, data: bytes):
        if len(data) < 8:
            raise ValueError("Truncated ExceptionInfo")
        self.vector, self.reserved, self.pid, self.error_code = \
            struct.unpack_from("<BBHI", data, 0)
        self.name = EXCEPTION_NAMES.get(self.vector, f"Unknown ({self.vector})")


class ProcessEntry:
    ENTRY_SIZE = 28  # 2+1+1+12+8 = 24... wait: 2+1+1+12+8=24

    def __init__(self, data: bytes, off: int = 0):
        self.pid, self.state, self.cpu = struct.unpack_from("<HBB", data, off)
        self.name = data[off+4:off+16].split(b'\x00')[0].decode('ascii', errors='replace')
        self.rip = struct.unpack_from("<Q", data, off+16)[0]
        self.state_name = PROCESS_STATE_NAMES.get(self.state, f"?{self.state}")
        self.cpu_str = str(self.cpu) if self.cpu != 0xFF else "-"


class ProcessList:
    def __init__(self, data: bytes):
        if len(data) < 1:
            raise ValueError("Truncated ProcessList")
        self.count = data[0]
        self.entries = []
        off = 1
        for _ in range(self.count):
            if off + 24 > len(data):
                break
            self.entries.append(ProcessEntry(data, off))
            off += 24


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
def vnc_capture(host_port: str, save_path: str = "vnc_crash_capture.png") -> str:
    """Capture a screenshot directly from VNC (no vncdotool needed)."""
    try:
        from PIL import Image
    except ImportError:
        raise RuntimeError("Pillow not installed: pip install Pillow")

    if ":" in host_port:
        host, port_str = host_port.rsplit(":", 1)
        port = int(port_str)
    else:
        host, port = host_port, 5900

    def recv_exact(sock, n):
        data = b""
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("VNC connection closed")
            data += chunk
        return data

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        sock.connect((host, port))

        # VNC handshake
        version = recv_exact(sock, 12)
        sock.send(b"RFB 003.008\n")

        # Security types
        num_types = struct.unpack("B", recv_exact(sock, 1))[0]
        recv_exact(sock, num_types)  # read types
        sock.send(bytes([1]))  # None auth

        # Security result
        result = struct.unpack(">I", recv_exact(sock, 4))[0]
        if result != 0:
            raise RuntimeError(f"VNC auth failed (result={result})")

        # ClientInit - shared=1
        sock.send(bytes([1]))

        # ServerInit
        header = recv_exact(sock, 24)
        w, h = struct.unpack(">HH", header[0:4])
        name_len = struct.unpack(">I", header[20:24])[0]
        recv_exact(sock, name_len)  # skip name

        # Set pixel format to 32bpp XRGB
        pfmt = struct.pack(">BxxxBBBBHHHBBBxxx",
            0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        sock.send(pfmt)

        # Set encodings - raw only
        sock.send(struct.pack(">BxHi", 2, 1, 0))

        # Request full framebuffer
        sock.send(struct.pack(">BxHHHH", 3, 0, 0, w, h))

        # Read response
        resp_type = struct.unpack("B", recv_exact(sock, 1))[0]
        recv_exact(sock, 1)  # padding
        num_rects = struct.unpack(">H", recv_exact(sock, 2))[0]

        img = Image.new("RGB", (w, h))
        pixels = img.load()

        for _ in range(num_rects):
            rx, ry, rw, rh, encoding = struct.unpack(">HHHHi", recv_exact(sock, 12))
            if encoding == 0:  # Raw
                data = recv_exact(sock, rw * rh * 4)
                for py in range(rh):
                    for px in range(rw):
                        off = (py * rw + px) * 4
                        b, g, r = data[off], data[off+1], data[off+2]
                        pixels[rx + px, ry + py] = (r, g, b)

        img.save(save_path)
        print(f"  VNC screenshot saved: {save_path}", file=sys.stderr)
    finally:
        sock.close()

    return scan_qr(save_path)


# ── ANSI colors ─────────────────────────────────────────────────────────────
class C:
    """ANSI color codes, disabled if not a terminal."""
    _active = sys.stdout.isatty()

    RESET   = "\033[0m"  if _active else ""
    BOLD    = "\033[1m"  if _active else ""
    DIM     = "\033[2m"  if _active else ""
    RED     = "\033[91m" if _active else ""
    GREEN   = "\033[92m" if _active else ""
    YELLOW  = "\033[93m" if _active else ""
    CYAN    = "\033[96m" if _active else ""
    WHITE   = "\033[97m" if _active else ""
    BG_RED  = "\033[41m" if _active else ""
    GREY    = "\033[90m" if _active else ""


# ── Pretty printing ─────────────────────────────────────────────────────────
W = 56  # box width

def _box_top():
    return f"  {C.DIM}┌{'─' * W}┐{C.RESET}"

def _box_mid():
    return f"  {C.DIM}├{'─' * W}┤{C.RESET}"

def _box_bot():
    return f"  {C.DIM}└{'─' * W}┘{C.RESET}"

def _box_line(text: str):
    return f"  {C.DIM}│{C.RESET} {text:<{W - 2}} {C.DIM}│{C.RESET}"


def print_report(hdr: PanicHeader, regs: CPURegs | None, trace: StackTrace | None,
                 sym: Symbolicator | None, raw_data: bytes, show_raw: bool,
                 exc_info: ExceptionInfo | None = None,
                 proc_list: ProcessList | None = None):
    bar = "═" * (W + 4)
    print(f"\n  {C.RED}{C.BOLD}{bar}{C.RESET}")
    print(f"  {C.RED}{C.BOLD}{'🔴 BROOK OS CRASH DUMP':^{W + 4}}{C.RESET}")
    print(f"  {C.DIM}{'Version: 0x%02X  Page: %d/%d' % (hdr.version, hdr.page + 1, hdr.page_count):^{W + 4}}{C.RESET}")
    print(f"  {C.RED}{C.BOLD}{bar}{C.RESET}\n")

    # Exception info
    if exc_info:
        print(f"  {C.RED}{C.BOLD}Exception:{C.RESET} {C.WHITE}#{exc_info.vector:03d} ({exc_info.name}){C.RESET}")
        print(f"  {C.YELLOW}Error Code:{C.RESET} {C.WHITE}0x{exc_info.error_code:08X}{C.RESET}  {C.YELLOW}PID:{C.RESET} {C.WHITE}{exc_info.pid}{C.RESET}\n")

    if regs:
        print(f"  {C.CYAN}{C.BOLD}CPU Registers:{C.RESET}")
        print(_box_top())
        # GPRs — two per line
        names = GPR_NAMES
        for i in range(0, len(names), 2):
            a, b = names[i], names[i + 1] if i + 1 < len(names) else None
            left = f"{C.YELLOW}{a:<6s}{C.RESET} {C.WHITE}0x{regs.gprs[a]:016X}{C.RESET}"
            if b:
                right = f"{C.YELLOW}{b:<6s}{C.RESET} {C.WHITE}0x{regs.gprs[b]:016X}{C.RESET}"
                print(_box_line(f"{left}  {right}"))
            else:
                print(_box_line(left))

        # RIP with symbolication
        rip_sym = ""
        if sym:
            name = sym.resolve(regs.extra['RIP'])
            if name:
                rip_sym = f"  {C.GREEN}{name}{C.RESET}"
        print(_box_line(f"{C.RED}{C.BOLD}{'RIP':<6s}{C.RESET} {C.WHITE}0x{regs.extra['RIP']:016X}{C.RESET}{rip_sym}"))
        print(_box_line(f"{C.YELLOW}{'RSP':<6s}{C.RESET} {C.WHITE}0x{regs.extra['RSP']:016X}{C.RESET}  {C.YELLOW}{'RBP':<6s}{C.RESET} {C.WHITE}0x{regs.extra['RBP']:016X}{C.RESET}"))
        print(_box_line(f"{C.YELLOW}{'RFLAGS':<6s}{C.RESET} {C.WHITE}0x{regs.extra['RFLAGS']:016X}{C.RESET}"))

        # Control registers
        print(_box_mid())
        print(_box_line(f"{C.CYAN}{'CR0':<6s}{C.RESET} {C.WHITE}0x{regs.crs['CR0']:016X}{C.RESET}  {C.CYAN}{'CR2':<6s}{C.RESET} {C.WHITE}0x{regs.crs['CR2']:016X}{C.RESET}"))
        print(_box_line(f"{C.CYAN}{'CR3':<6s}{C.RESET} {C.WHITE}0x{regs.crs['CR3']:016X}{C.RESET}  {C.CYAN}{'CR4':<6s}{C.RESET} {C.WHITE}0x{regs.crs['CR4']:016X}{C.RESET}"))

        # Segment registers
        seg_parts = [f"{C.DIM}{n}{C.RESET}={C.WHITE}0x{regs.segs[n]:04X}{C.RESET}" for n in SEG_NAMES]
        print(_box_line("  ".join(seg_parts)))
        print(_box_bot())

    if trace:
        print(f"\n  {C.CYAN}{C.BOLD}Stack Trace ({len(trace.frames)} frames):{C.RESET}")
        for i, addr in enumerate(trace.frames):
            sym_str = ""
            loc_str = ""
            if sym:
                name = sym.resolve(addr)
                if name:
                    sym_str = f"  {C.GREEN}{name}{C.RESET}"
                    loc = sym.addr2line(addr)
                    if loc:
                        loc_str = f"  {C.DIM}{loc}{C.RESET}"
            if not sym_str and addr > 0x7FFF00000000:
                sym_str = f"  {C.DIM}(userspace){C.RESET}"
            prefix = f"{C.RED}→{C.RESET}" if i == 0 else " "
            print(f"  {prefix} {C.YELLOW}#{i:02d}{C.RESET}  {C.WHITE}0x{addr:016X}{C.RESET}{sym_str}{loc_str}")

    if show_raw:
        print(f"\n  {C.CYAN}{C.BOLD}Raw hex dump ({len(raw_data)} bytes):{C.RESET}")
        for off in range(0, len(raw_data), 16):
            chunk = raw_data[off:off + 16]
            hex_part = " ".join(f"{b:02X}" for b in chunk)
            ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print(f"  {C.DIM}{off:04X}{C.RESET}  {hex_part:<48s}  {C.DIM}{ascii_part}{C.RESET}")

    if proc_list and proc_list.entries:
        print(f"\n  {C.CYAN}{C.BOLD}Running Processes ({len(proc_list.entries)}):{C.RESET}")
        print(f"  {C.DIM}{'PID':>5s}  {'STATE':<10s}  {'CPU':>3s}  {'NAME':<12s}  {'RIP'}{C.RESET}")
        for pe in proc_list.entries:
            rip_str = f"0x{pe.rip:016X}" if pe.rip else ""
            state_color = C.GREEN if pe.state_name == "Running" else C.YELLOW if pe.state_name == "Ready" else C.DIM
            print(f"  {C.WHITE}{pe.pid:5d}{C.RESET}  {state_color}{pe.state_name:<10s}{C.RESET}  {C.WHITE}{pe.cpu_str:>3s}{C.RESET}  {C.CYAN}{pe.name:<12s}{C.RESET}  {C.DIM}{rip_str}{C.RESET}")

    print(f"\n  {C.RED}{C.BOLD}{bar}{C.RESET}\n")


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


# ── Auto-detect ELF ─────────────────────────────────────────────────────────
def find_brook_elf() -> str | None:
    """Search common locations for BROOK.elf."""
    script_dir = Path(__file__).parent.parent
    candidates = [
        script_dir / "build" / "debug" / "kernel" / "BROOK.elf",
        script_dir / "build" / "release" / "kernel" / "BROOK.elf",
        script_dir / "build" / "kernel" / "BROOK.elf",
        Path("BROOK.elf"),
    ]
    for p in candidates:
        if p.exists():
            return str(p)
    return None


# ── Main ────────────────────────────────────────────────────────────────────
def decode_crash(data: bytes, sym: Symbolicator | None,
                 show_raw: bool, as_json: bool):
    hdr = PanicHeader(data)
    hdr.validate()

    off = PanicHeader.SIZE
    regs = None
    trace = None
    exc_info = None
    proc_list = None

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
        elif pkt.type == PKT_EXCEPTION_INFO:
            exc_info = ExceptionInfo(payload)
        elif pkt.type == PKT_PROCESS_LIST:
            proc_list = ProcessList(payload)
        else:
            print(f"[warn] Unknown packet type 0x{pkt.type:08X} ({pkt.size}B)",
                  file=sys.stderr)

        off = payload_end

    if as_json:
        print(json.dumps(build_json(hdr, regs, trace, sym), indent=2))
    else:
        print_report(hdr, regs, trace, sym, data, show_raw,
                     exc_info=exc_info, proc_list=proc_list)


def main():
    ap = argparse.ArgumentParser(
        description="Brook OS crash decoder — decode QR panic dumps",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  %(prog)s screenshot.png                 # Decode QR from image
  %(prog)s --vnc localhost:5943           # Live VNC capture + decode
  %(prog)s --base45 "1A2B..."            # Decode raw Base45 text
  %(prog)s screenshot.png --raw --json   # JSON output with hex dump
""")
    ap.add_argument("image", nargs="?", help="Path to screenshot image with QR code")
    ap.add_argument("--base45", metavar="TEXT", help="Decode raw Base45 text directly")
    ap.add_argument("--elf", metavar="PATH", help="Path to BROOK.elf for symbolication")
    ap.add_argument("--vnc", metavar="HOST:PORT", help="Capture from VNC server")
    ap.add_argument("--raw", action="store_true", help="Show raw hex dump")
    ap.add_argument("--json", action="store_true", help="Output as JSON")
    ap.add_argument("--no-color", action="store_true", help="Disable ANSI colors")
    args = ap.parse_args()

    if args.no_color:
        for attr in dir(C):
            if not attr.startswith("_"):
                setattr(C, attr, "")

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
        print(f"{C.RED}[error]{C.RESET} Base45 decode failed: {e}", file=sys.stderr)
        sys.exit(1)

    # Symbolicator — auto-detect ELF if not specified
    elf_path = args.elf or find_brook_elf()
    sym = None
    if elf_path:
        sym = Symbolicator(elf_path)
        if sym.symbols:
            print(f"  {C.GREEN}✓{C.RESET} Loaded {len(sym.symbols)} symbols from {elf_path}",
                  file=sys.stderr)
    else:
        print(f"  {C.YELLOW}⚠{C.RESET} No BROOK.elf found — use --elf for symbolication",
              file=sys.stderr)

    decode_crash(raw, sym, args.raw, args.json)


if __name__ == "__main__":
    main()

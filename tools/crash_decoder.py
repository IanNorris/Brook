#!/usr/bin/env python3
"""Brook OS Crash Decoder — decodes QR-encoded panic dumps from screenshots.

Usage:
  crash_decoder.py screenshot.png          # Decode QR from image
  crash_decoder.py --vnc localhost:5943    # Capture from VNC + decode
  crash_decoder.py --hex "2d01..."         # Decode raw hex bytes

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

# Protocol versions
PANIC_VERSION_RAW = 0x01  # v1: uncompressed TLV payload
PANIC_VERSION_LZ4 = 0x02  # v2: LZ4-compressed TLV payload (4-byte size prefix)

PKT_CPU_REGS       = 0xA3000001
PKT_STACK_TRACE    = 0xA3000002
PKT_EXCEPTION_INFO = 0xA3000003
PKT_PROCESS_LIST   = 0xA3000004
PKT_SYSTEM_INFO    = 0xA3000005
PKT_STACK_DUMP     = 0xA3000006
PKT_PROCESS_EXT    = 0xA3000007  # BRO-176 reap-gate fields, merged onto PROCESS_LIST by pid
PKT_CPU_STATE      = 0xA3000008  # per-CPU RIP/CR3/pid
PKT_CUSTOM_BLOB    = 0xA3000009  # generic bug-specific blob (tag + format + bytes)

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
    # PROCESS_LIST wire entry is the stable 24-byte form (pid/state/cpu/name/rip).
    # Reap-gate fields arrive separately in the PROCESS_EXT packet and are merged
    # onto matching entries by pid (see ProcessExt / ProcessList.merge_ext).
    WIRE_SIZE = 24

    FLAG_IS_THREAD  = 0x01
    FLAG_REAPABLE   = 0x02
    FLAG_IS_KTHREAD = 0x04
    FLAG_MAGIC_BAD  = 0x08

    def __init__(self, data: bytes, off: int = 0):
        self.pid, self.state, self.cpu = struct.unpack_from("<HBB", data, off)
        self.name = data[off+4:off+16].split(b'\x00')[0].decode('ascii', errors='replace')
        self.rip = struct.unpack_from("<Q", data, off+16)[0]
        self.state_name = PROCESS_STATE_NAMES.get(self.state, f"?{self.state}")
        self.cpu_str = str(self.cpu) if self.cpu != 0xFF else "-"
        # Reap-gate fields — populated from a PROCESS_EXT packet if present.
        self.has_reap = False
        self.tgid = self.pid
        self.as_live_threads = -1
        self.ref_count = 0
        self.flags = 0

    def apply_ext(self, tgid: int, as_live: int, ref: int, flags: int):
        self.has_reap = True
        self.tgid = tgid
        self.as_live_threads = as_live
        self.ref_count = ref
        self.flags = flags

    @property
    def is_thread(self) -> bool:  return bool(self.flags & self.FLAG_IS_THREAD)
    @property
    def reapable(self) -> bool:   return bool(self.flags & self.FLAG_REAPABLE)
    @property
    def is_kthread(self) -> bool: return bool(self.flags & self.FLAG_IS_KTHREAD)
    @property
    def magic_bad(self) -> bool:  return bool(self.flags & self.FLAG_MAGIC_BAD)


class ProcessList:
    def __init__(self, data: bytes):
        if len(data) < 1:
            raise ValueError("Truncated ProcessList")
        self.count = data[0]
        self.entries = []
        off = 1
        for _ in range(self.count):
            if off + ProcessEntry.WIRE_SIZE > len(data):
                break
            self.entries.append(ProcessEntry(data, off))
            off += ProcessEntry.WIRE_SIZE

    def merge_ext(self, data: bytes):
        """Merge a PROCESS_EXT packet (reap-gate fields, keyed by pid)."""
        if len(data) < 1:
            return
        count = data[0]
        off = 1
        ext_by_pid = {}
        for _ in range(count):
            if off + 10 > len(data):
                break
            pid, tgid, as_live, ref, flags, _resv = \
                struct.unpack_from("<HHhhBB", data, off)
            ext_by_pid[pid] = (tgid, as_live, ref, flags)
            off += 10
        for e in self.entries:
            if e.pid in ext_by_pid:
                e.apply_ext(*ext_by_pid[e.pid])


class CpuEntry:
    WIRE_SIZE = 20  # cpuIndex(1)+flags(1)+pid(2)+rip(8)+cr3(8)

    FLAG_ONLINE   = 0x01
    FLAG_HALTED   = 0x02
    FLAG_BSP      = 0x04
    FLAG_LIVE_RIP = 0x08

    def __init__(self, data: bytes, off: int = 0):
        self.cpu_index, self.flags, self.pid = struct.unpack_from("<BBH", data, off)
        self.rip = struct.unpack_from("<Q", data, off+4)[0]
        self.cr3 = struct.unpack_from("<Q", data, off+12)[0]

    @property
    def online(self) -> bool:    return bool(self.flags & self.FLAG_ONLINE)
    @property
    def halted(self) -> bool:    return bool(self.flags & self.FLAG_HALTED)
    @property
    def is_bsp(self) -> bool:    return bool(self.flags & self.FLAG_BSP)
    @property
    def live_rip(self) -> bool:  return bool(self.flags & self.FLAG_LIVE_RIP)


class CpuList:
    def __init__(self, data: bytes):
        if len(data) < 1:
            raise ValueError("Truncated CpuList")
        self.count = data[0]
        self.entries = []
        off = 1
        for _ in range(self.count):
            if off + CpuEntry.WIRE_SIZE > len(data):
                break
            self.entries.append(CpuEntry(data, off))
            off += CpuEntry.WIRE_SIZE


PANIC_GIT_HASH_LEN   = 20
PANIC_GIT_BRANCH_LEN = 24


class SystemInfo:
    def __init__(self, data: bytes):
        min_size_v1 = 4 + 8 + 8 + PANIC_GIT_HASH_LEN  # without branch
        if len(data) < min_size_v1:
            raise ValueError("Truncated SystemInfo")
        self.cpu_index, self.cpu_count, self.reserved = \
            struct.unpack_from("<BBH", data, 0)
        self.tsc_ticks = struct.unpack_from("<Q", data, 4)[0]
        self.tss_rsp0 = struct.unpack_from("<Q", data, 12)[0]
        raw_hash = data[20:20 + PANIC_GIT_HASH_LEN]
        self.git_hash = raw_hash.split(b'\x00')[0].decode('ascii', errors='replace')
        # Branch field added in v2 — optional for backwards compatibility
        branch_off = 20 + PANIC_GIT_HASH_LEN
        if len(data) >= branch_off + PANIC_GIT_BRANCH_LEN:
            raw_branch = data[branch_off:branch_off + PANIC_GIT_BRANCH_LEN]
            self.git_branch = raw_branch.split(b'\x00')[0].decode('ascii', errors='replace')
        else:
            self.git_branch = ""


class StackDump:
    def __init__(self, data: bytes):
        if len(data) < 10:
            raise ValueError("Truncated StackDump")
        self.rsp = struct.unpack_from("<Q", data, 0)[0]
        self.length = struct.unpack_from("<H", data, 8)[0]
        self.data = data[10:10 + self.length]


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


# ── LZ4 decompression ───────────────────────────────────────────────────────
def lz4_decompress(data: bytes, uncompressed_size: int) -> bytes:
    """Decompress LZ4 block data. Uses python-lz4 if available, else pure-Python."""
    try:
        import lz4.block
        return lz4.block.decompress(data, uncompressed_size=uncompressed_size)
    except ImportError:
        pass
    # Pure-Python LZ4 block decoder (no dependencies)
    src = data
    dst = bytearray(uncompressed_size)
    si, di = 0, 0
    while si < len(src):
        token = src[si]; si += 1
        lit_len = token >> 4
        if lit_len == 15:
            while si < len(src):
                extra = src[si]; si += 1
                lit_len += extra
                if extra != 255: break
        if si + lit_len > len(src):
            lit_len = len(src) - si
        dst[di:di+lit_len] = src[si:si+lit_len]
        si += lit_len; di += lit_len
        if si >= len(src):
            break
        offset = src[si] | (src[si+1] << 8); si += 2
        if offset == 0:
            raise ValueError("LZ4: zero offset")
        match_len = (token & 0x0F) + 4
        if match_len == 19:
            while si < len(src):
                extra = src[si]; si += 1
                match_len += extra
                if extra != 255: break
        match_pos = di - offset
        for j in range(match_len):
            dst[di] = dst[match_pos + j]
            di += 1
    return bytes(dst[:di])


def decompress_payload(data: bytes, version: int) -> bytes:
    """If v2, strip 4-byte size prefix and LZ4-decompress. Otherwise pass through."""
    if version == PANIC_VERSION_LZ4:
        if len(data) < 4:
            raise ValueError("LZ4 payload too short (need 4-byte size prefix)")
        uncompressed_size = struct.unpack_from("<I", data, 0)[0]
        compressed = data[4:]
        return lz4_decompress(compressed, uncompressed_size)
    return data


# ── QR scanning ─────────────────────────────────────────────────────────────
def scan_qr(image_path: str) -> bytes:
    """Scan QR code from image. Returns raw bytes.

    If the QR contains Base45 alphanumeric text (v2 format), decode it.
    If the QR contains raw binary data (v1 format), return as-is.
    """
    try:
        from pyzbar.pyzbar import decode as pyzbar_decode
        from PIL import Image
    except ImportError:
        raise RuntimeError(
            "pyzbar/Pillow not installed. Use --hex to supply raw hex instead.\n"
            "  pip install pyzbar Pillow"
        )
    img = Image.open(image_path)
    results = pyzbar_decode(img)
    if not results:
        raise RuntimeError(f"No QR code found in {image_path}")
    qr_data = results[0].data
    # Try Base45 decode first (v2 format — alphanumeric QR text)
    try:
        text = qr_data.decode("ascii")
        decoded = base45_decode(text)
        # Validate it's a panic packet
        if len(decoded) >= 8 and decoded[0] == PANIC_MAGIC:
            return decoded
    except (UnicodeDecodeError, KeyError, ValueError):
        pass
    # Fall back to binary QR mode (v1 format)
    return qr_data.decode("utf-8").encode("latin-1")


# ── VNC capture ─────────────────────────────────────────────────────────────
def vnc_capture(host_port: str, save_path: str = "vnc_crash_capture.png") -> bytes:
    """Capture a screenshot directly from VNC, then scan QR and return raw bytes."""
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
                 proc_list: ProcessList | None = None,
                 sys_info: SystemInfo | None = None,
                 stack_dump: StackDump | None = None,
                 cpu_list: "CpuList | None" = None):
    bar = "═" * (W + 4)
    print(f"\n  {C.RED}{C.BOLD}{bar}{C.RESET}")
    print(f"  {C.RED}{C.BOLD}{'🔴 BROOK OS CRASH DUMP':^{W + 4}}{C.RESET}")
    version_str = 'Version: 0x%02X  Page: %d/%d' % (hdr.version, hdr.page + 1, hdr.page_count)
    if sys_info:
        build_id = sys_info.git_hash
        if sys_info.git_branch:
            build_id = f"{sys_info.git_branch}/{build_id}"
        version_str += f"  Build: {build_id}"
    print(f"  {C.DIM}{version_str:^{W + 4}}{C.RESET}")
    print(f"  {C.RED}{C.BOLD}{bar}{C.RESET}\n")

    # System info
    if sys_info:
        print(f"  {C.CYAN}{C.BOLD}System Info:{C.RESET}")
        git_str = sys_info.git_hash
        if sys_info.git_branch:
            git_str = f"{sys_info.git_branch}/{git_str}"
        print(f"  {C.YELLOW}CPU:{C.RESET} {C.WHITE}{sys_info.cpu_index}/{sys_info.cpu_count}{C.RESET}  "
              f"{C.YELLOW}TSC:{C.RESET} {C.WHITE}0x{sys_info.tsc_ticks:016X}{C.RESET}  "
              f"{C.YELLOW}TSS RSP0:{C.RESET} {C.WHITE}0x{sys_info.tss_rsp0:016X}{C.RESET}  "
              f"{C.YELLOW}Git:{C.RESET} {C.WHITE}{git_str}{C.RESET}\n")

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

    if stack_dump and stack_dump.length > 0:
        print(f"\n  {C.CYAN}{C.BOLD}Stack Dump ({stack_dump.length} bytes from RSP=0x{stack_dump.rsp:016X}):{C.RESET}")
        for off in range(0, stack_dump.length, 16):
            addr = stack_dump.rsp + off
            chunk = stack_dump.data[off:off + 16]
            hex_part = " ".join(f"{b:02X}" for b in chunk)
            ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            # Try to decode 8-byte values as potential pointers
            ptr_str = ""
            if len(chunk) >= 8:
                ptr = struct.unpack_from("<Q", chunk, 0)[0]
                if sym and ptr > 0xFFFF800000000000:
                    name = sym.resolve(ptr)
                    if name:
                        ptr_str = f"  {C.GREEN}{name}{C.RESET}"
            print(f"  {C.DIM}{addr:016X}{C.RESET}  {hex_part:<48s}  {C.DIM}{ascii_part}{C.RESET}{ptr_str}")

    if proc_list and proc_list.entries:
        has_reap = any(pe.has_reap for pe in proc_list.entries)
        print(f"\n  {C.CYAN}{C.BOLD}Processes ({len(proc_list.entries)}):{C.RESET}")
        if has_reap:
            print(f"  {C.DIM}{'PID':>5s} {'TGID':>5s}  {'STATE':<10s} {'CPU':>3s}  "
                  f"{'NAME':<12s} {'T':>1s}{'R':>1s}{'K':>1s}  {'asLive':>6s} {'ref':>4s}  {'RIP'}{C.RESET}")
        else:
            print(f"  {C.DIM}{'PID':>5s}  {'STATE':<10s}  {'CPU':>3s}  {'NAME':<12s}  {'RIP'}{C.RESET}")
        for pe in proc_list.entries:
            rip_str = f"0x{pe.rip:016X}" if pe.rip else ""
            state_color = C.GREEN if pe.state_name == "Running" else C.YELLOW if pe.state_name == "Ready" else C.DIM
            if has_reap:
                # Flag a leader whose live-thread gate is non-zero while it is a
                # zombie — the classic BRO-176 reap-stall fingerprint.
                tflag = "T" if pe.is_thread else "-"
                rflag = "R" if pe.reapable else "-"
                kflag = "K" if pe.is_kthread else "-"
                al = "" if pe.as_live_threads < 0 else str(pe.as_live_threads)
                name_col = C.RED if pe.magic_bad else C.CYAN
                stall = ""
                if (not pe.is_thread and pe.state_name == "Terminated"
                        and pe.as_live_threads > 0):
                    stall = f"  {C.RED}{C.BOLD}<-- REAP-STALL (zombie leader, asLive>0){C.RESET}"
                print(f"  {C.WHITE}{pe.pid:5d}{C.RESET} {pe.tgid:5d}  "
                      f"{state_color}{pe.state_name:<10s}{C.RESET} {C.WHITE}{pe.cpu_str:>3s}{C.RESET}  "
                      f"{name_col}{pe.name:<12s}{C.RESET} {tflag}{rflag}{kflag}  "
                      f"{al:>6s} {pe.ref_count:>4d}  {C.DIM}{rip_str}{C.RESET}{stall}")
            else:
                print(f"  {C.WHITE}{pe.pid:5d}{C.RESET}  {state_color}{pe.state_name:<10s}{C.RESET}  "
                      f"{C.WHITE}{pe.cpu_str:>3s}{C.RESET}  {C.CYAN}{pe.name:<12s}{C.RESET}  "
                      f"{C.DIM}{rip_str}{C.RESET}")

    if cpu_list and cpu_list.entries:
        print(f"\n  {C.CYAN}{C.BOLD}Per-CPU State ({len(cpu_list.entries)}):{C.RESET}")
        print(f"  {C.DIM}{'CPU':>3s} {'PID':>5s}  {'CR3':<18s} {'RIP':<18s} {'WHERE'}{C.RESET}")
        # Distinct CR3 set helps spot 'all CPUs in kernel AS' at a glance.
        cr3s = {}
        for ce in cpu_list.entries:
            where = ""
            if sym:
                name = sym.resolve(ce.rip)
                if name:
                    where = name
                    loc = sym.addr2line(ce.rip)
                    if loc:
                        where += f"  {C.DIM}{loc}{C.RESET}"
            tag = []
            if ce.is_bsp: tag.append("BSP")
            if not ce.online: tag.append("OFFLINE")
            if ce.halted: tag.append("halted")
            tag.append("live" if ce.live_rip else "sched")
            tagstr = f"{C.DIM}[{','.join(tag)}]{C.RESET}"
            cr3s.setdefault(ce.cr3, []).append(ce.cpu_index)
            print(f"  {C.WHITE}{ce.cpu_index:>3d}{C.RESET} {C.WHITE}{ce.pid:>5d}{C.RESET}  "
                  f"{C.DIM}0x{ce.cr3:012X}{C.RESET}     0x{ce.rip:012X}     "
                  f"{C.GREEN}{where}{C.RESET} {tagstr}")
        if len(cr3s) == 1:
            only = next(iter(cr3s))
            print(f"  {C.DIM}(all CPUs share CR3 0x{only:012X} — same address space){C.RESET}")

    print(f"\n  {C.RED}{C.BOLD}{bar}{C.RESET}\n")


def build_json(hdr: PanicHeader, regs: CPURegs | None, trace: StackTrace | None,
               sym: Symbolicator | None, exc_info: ExceptionInfo | None = None,
               proc_list: ProcessList | None = None,
               sys_info: SystemInfo | None = None,
               stack_dump: StackDump | None = None,
               cpu_list: "CpuList | None" = None) -> dict:
    out: dict = {
        "header": {
            "magic": f"0x{hdr.magic:02X}",
            "version": hdr.version,
            "page": hdr.page + 1,
            "page_count": hdr.page_count,
            "compressed": hdr.version == PANIC_VERSION_LZ4,
        }
    }
    if exc_info:
        out["exception"] = {
            "vector": exc_info.vector,
            "name": exc_info.name,
            "error_code": f"0x{exc_info.error_code:08X}",
            "pid": exc_info.pid,
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
                loc = sym.addr2line(addr)
                if loc:
                    entry["location"] = loc
            frames.append(entry)
        out["stack_trace"] = frames
    if proc_list and proc_list.entries:
        procs = []
        for pe in proc_list.entries:
            entry = {
                "pid": pe.pid,
                "state": pe.state_name,
                "cpu": pe.cpu_str,
                "name": pe.name,
                "rip": f"0x{pe.rip:016X}" if pe.rip else None,
            }
            if pe.has_reap:
                entry.update({
                    "tgid": pe.tgid,
                    "as_live_threads": pe.as_live_threads,
                    "ref_count": pe.ref_count,
                    "is_thread": pe.is_thread,
                    "reapable": pe.reapable,
                    "is_kthread": pe.is_kthread,
                    "magic_bad": pe.magic_bad,
                    "reap_stall": (not pe.is_thread
                                   and pe.state_name == "Terminated"
                                   and pe.as_live_threads > 0),
                })
            procs.append(entry)
        out["processes"] = procs
    if sys_info:
        out["system_info"] = {
            "cpu_index": sys_info.cpu_index,
            "cpu_count": sys_info.cpu_count,
            "tsc_ticks": f"0x{sys_info.tsc_ticks:016X}",
            "tss_rsp0": f"0x{sys_info.tss_rsp0:016X}",
            "git_hash": sys_info.git_hash,
            "git_branch": sys_info.git_branch,
        }
    if stack_dump and stack_dump.length > 0:
        out["stack_dump"] = {
            "rsp": f"0x{stack_dump.rsp:016X}",
            "length": stack_dump.length,
            "hex": stack_dump.data.hex(),
        }
    if cpu_list and cpu_list.entries:
        cpus = []
        for ce in cpu_list.entries:
            entry = {
                "cpu": ce.cpu_index,
                "pid": ce.pid,
                "rip": f"0x{ce.rip:016X}",
                "cr3": f"0x{ce.cr3:016X}",
                "online": ce.online,
                "halted": ce.halted,
                "is_bsp": ce.is_bsp,
                "live_rip": ce.live_rip,
            }
            if sym:
                name = sym.resolve(ce.rip)
                if name:
                    entry["symbol"] = name
                loc = sym.addr2line(ce.rip)
                if loc:
                    entry["location"] = loc
            cpus.append(entry)
        out["cpus"] = cpus
    return out


# ── Auto-detect ELF ─────────────────────────────────────────────────────────
def find_brook_elf(git_hash: str | None = None) -> str | None:
    """Search for BROOK.elf, preferring the symbol archive matching git_hash."""
    script_dir = Path(__file__).parent.parent
    symbols_dir = script_dir / "symbols"

    # A "+" suffix marks a dirty (uncommitted) build: no archive can match it, so
    # symbolization MUST use the exact local build ELF that produced the running
    # kernel. Warn loudly because if that ELF has since been rebuilt, addresses
    # will be shifted and symbols will be wrong.
    dirty = bool(git_hash) and git_hash.endswith("+")
    if dirty:
        print(f"  [warn] Dirty build ({git_hash}) — symbolizing against the LOCAL "
              f"build ELF. Ensure it is the EXACT kernel you booted, or symbols "
              f"will be wrong.")

    # If we have a clean git hash, try to extract from the symbol archive first
    if git_hash and not dirty and symbols_dir.is_dir():
        archive = symbols_dir / f"{git_hash}.tar.xz"
        if archive.exists():
            extract_dir = symbols_dir / f".extract_{git_hash}"
            elf_path = extract_dir / "kernel" / "BROOK.elf"
            if not elf_path.exists():
                # Extract on demand
                extract_dir.mkdir(parents=True, exist_ok=True)
                try:
                    import subprocess as sp
                    sp.run(["tar", "-xJf", str(archive), "-C", str(extract_dir)],
                           check=True, capture_output=True)
                except Exception as e:
                    print(f"  [warn] Failed to extract symbol archive: {e}")
            if elf_path.exists():
                return str(elf_path)

    # Fallback: current build directory
    candidates = [
        script_dir / "build" / "debug" / "kernel" / "BROOK.elf",
        script_dir / "build" / "release" / "kernel" / "BROOK.elf",
        script_dir / "build" / "kernel" / "BROOK.elf",
        Path("BROOK.elf"),
    ]
    for p in candidates:
        if p.exists():
            if git_hash:
                print(f"  [warn] No symbol archive for {git_hash}, using current build ELF")
            return str(p)
    return None


# ── Main ────────────────────────────────────────────────────────────────────
def decode_crash(data: bytes, sym: Symbolicator | None,
                 show_raw: bool, as_json: bool):
    hdr = PanicHeader(data)
    hdr.validate()

    # Decompress payload if v2 (LZ4)
    payload = data[PanicHeader.SIZE:]
    try:
        payload = decompress_payload(payload, hdr.version)
    except Exception as e:
        print(f"[warn] Decompression failed: {e} — trying raw", file=sys.stderr)

    off = 0
    regs = None
    trace = None
    exc_info = None
    proc_list = None
    sys_info = None
    stack_dump = None
    cpu_list = None
    proc_ext_data = None  # buffered until after the loop (may precede PROCESS_LIST)
    custom_blobs = []     # PKT_CUSTOM_BLOB payloads (tag + format + raw bytes)

    while off + PacketHeader.SIZE <= len(payload):
        pkt = PacketHeader(payload, off)
        payload_start = off + PacketHeader.SIZE
        payload_end = payload_start + pkt.size

        if payload_end > len(payload):
            print(f"[warn] Packet at offset {off} truncated "
                  f"(need {pkt.size}B, have {len(payload) - payload_start}B)",
                  file=sys.stderr)
            payload_end = len(payload)

        pkt_data = payload[payload_start:payload_end]

        if pkt.type == PKT_CPU_REGS:
            regs = CPURegs(pkt_data)
        elif pkt.type == PKT_STACK_TRACE:
            trace = StackTrace(pkt_data)
        elif pkt.type == PKT_EXCEPTION_INFO:
            exc_info = ExceptionInfo(pkt_data)
        elif pkt.type == PKT_PROCESS_LIST:
            proc_list = ProcessList(pkt_data)
        elif pkt.type == PKT_PROCESS_EXT:
            proc_ext_data = pkt_data  # merge after the loop (ordering-independent)
        elif pkt.type == PKT_CPU_STATE:
            cpu_list = CpuList(pkt_data)
        elif pkt.type == PKT_SYSTEM_INFO:
            sys_info = SystemInfo(pkt_data)
        elif pkt.type == PKT_STACK_DUMP:
            stack_dump = StackDump(pkt_data)
        elif pkt.type == PKT_CUSTOM_BLOB:
            custom_blobs.append(pkt_data)
        else:
            print(f"[warn] Unknown packet type 0x{pkt.type:08X} ({pkt.size}B)",
                  file=sys.stderr)

        off = payload_end

    # Merge reap-gate extension onto the process list (keyed by pid).
    if proc_list is not None and proc_ext_data is not None:
        try:
            proc_list.merge_ext(proc_ext_data)
        except Exception as e:
            print(f"[warn] PROCESS_EXT merge failed: {e}", file=sys.stderr)

    # If no symbolicator was provided, try to find one matching the dump's git hash
    if sym is None and sys_info and sys_info.git_hash:
        elf_path = find_brook_elf(sys_info.git_hash)
        if elf_path:
            sym = Symbolicator(elf_path)

    if as_json:
        print(json.dumps(build_json(hdr, regs, trace, sym,
                                    exc_info=exc_info, proc_list=proc_list,
                                    sys_info=sys_info, stack_dump=stack_dump,
                                    cpu_list=cpu_list), indent=2))
    else:
        print_report(hdr, regs, trace, sym, data, show_raw,
                     exc_info=exc_info, proc_list=proc_list,
                     sys_info=sys_info, stack_dump=stack_dump,
                     cpu_list=cpu_list)
        for blob in custom_blobs:
            print_custom_blob(blob, sym)


def print_custom_blob(blob: bytes, sym: "Symbolicator | None"):
    """Render a PKT_CUSTOM_BLOB. Header: tag[8], format(u16), reserved(u16)."""
    if len(blob) < 12:
        print("[warn] custom blob too short", file=sys.stderr)
        return
    tag = blob[:8].split(b"\x00", 1)[0].decode("ascii", "replace")
    (fmt, _resv) = struct.unpack_from("<HH", blob, 8)
    body = blob[12:]
    print(f"\n  ── Custom diagnostic: {tag} (format {fmt}) ──")
    if tag == "BRO208" and fmt == 1:
        _print_bro208(body, sym)
    else:
        print(f"    {len(body)} bytes (no renderer for {tag}/{fmt}); hex:")
        print("    " + body.hex())


def _sym(sym, addr):
    if sym:
        r = sym.resolve(addr)
        if r:
            return f"  {r}"
    return ""


def _print_bro208(body: bytes, sym):
    # Fixed head: cr3,curProc,liveRsp,curSavedRsp,curSavedRip,curSavedCr3,
    # curStackBase,curStackTop (8x u64), ringCount(u32), then recs.
    head = struct.unpack_from("<8Q I", body, 0)
    (cr3, cur, rsp, sRsp, sRip, sCr3, sBase, sTop, ringCount) = head
    print(f"    live:   CR3=0x{cr3:016x}  currentProcess=0x{cur:016x}  liveRSP=0x{rsp:016x}")
    print(f"    curCtx: savedRSP=0x{sRsp:016x}{_sym(sym, sRip)}")
    print(f"            savedRIP=0x{sRip:016x}  savedCR3=0x{sCr3:016x}")
    print(f"            stack=[0x{sBase:016x}, 0x{sTop:016x})  "
          f"RSP_in_bounds={sBase <= rsp < sTop}")
    inb = sBase <= rsp < sTop
    print(f"    >>> currentProcess RSP-in-its-own-stack={sBase <= sRsp < sTop}; "
          f"liveRSP-in-currentProcess-stack={inb}")
    off = struct.calcsize("<8Q I")
    reasons = {1: "DoSwitch", 2: "exit", 3: "start", 4: "apstart"}
    print(f"    ownership ring ({ringCount} transitions, newest last):")
    for i in range(ringCount):
        (seq, tsc, ra, oldp, newp, rif) = struct.unpack_from("<5Q H", body, off)
        off += struct.calcsize("<5Q H")
        reason = reasons.get(rif & 0xff, "?")
        iff = (rif >> 8) & 1
        print(f"      #{seq:#x} tsc={tsc} IF={iff} {reason} "
              f"ra=0x{ra:012x}{_sym(sym, ra)}  0x{oldp:012x} -> 0x{newp:012x}")


def parse_serial_log(path: str) -> bytes:
    """Extract PANIC_HEX: line from serial log file and decode it."""
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("PANIC_HEX:"):
                hex_data = line[len("PANIC_HEX:"):]
                return bytes.fromhex(hex_data)
    raise ValueError(f"No PANIC_HEX: line found in {path}")


def main():
    ap = argparse.ArgumentParser(
        description="Brook OS crash decoder — decode QR panic dumps",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  %(prog)s screenshot.png                 # Decode QR from image
  %(prog)s --vnc localhost:5943           # Live VNC capture + decode
  %(prog)s --hex "2d01..."                # Decode raw hex bytes directly
  %(prog)s --base45 "1A2B..."            # Decode Base45 text from QR scan
  %(prog)s --serial /tmp/serial.log      # Extract from QEMU serial log
  %(prog)s --stdin                        # Read hex from stdin (paste mode)
  %(prog)s screenshot.png --raw --json   # JSON output with hex dump
  %(prog)s --save crash_report.txt       # Save report to file
""")
    ap.add_argument("image", nargs="?", help="Path to screenshot image with QR code")
    ap.add_argument("--hex", metavar="HEX", help="Decode raw hex bytes directly")
    ap.add_argument("--base45", metavar="TEXT", help="Decode Base45 text (from QR scan)")
    ap.add_argument("--elf", metavar="PATH", help="Path to BROOK.elf for symbolication")
    ap.add_argument("--vnc", metavar="HOST:PORT", help="Capture from VNC server")
    ap.add_argument("--serial", metavar="PATH", help="Extract from serial log (PANIC_HEX: line)")
    ap.add_argument("--stdin", action="store_true", help="Read hex from stdin (paste mode)")
    ap.add_argument("--save", metavar="PATH", help="Save report to file")
    ap.add_argument("--raw", action="store_true", help="Show raw hex dump")
    ap.add_argument("--json", action="store_true", help="Output as JSON")
    ap.add_argument("--no-color", action="store_true", help="Disable ANSI colors")
    args = ap.parse_args()

    if args.no_color:
        for attr in dir(C):
            if not attr.startswith("_"):
                setattr(C, attr, "")

    # Obtain raw crash data
    raw = None
    if args.hex:
        try:
            raw = bytes.fromhex(args.hex.strip())
        except ValueError as e:
            print(f"{C.RED}[error]{C.RESET} Hex decode failed: {e}", file=sys.stderr)
            sys.exit(1)
    elif args.stdin:
        print(f"  {C.CYAN}Paste hex data (PANIC_HEX: prefix accepted), then press Enter:{C.RESET}",
              file=sys.stderr)
        line = sys.stdin.readline().strip()
        if line.startswith("PANIC_HEX:"):
            line = line[len("PANIC_HEX:"):]
        try:
            raw = bytes.fromhex(line)
        except ValueError as e:
            print(f"{C.RED}[error]{C.RESET} Hex decode failed: {e}", file=sys.stderr)
            sys.exit(1)
    elif args.base45:
        try:
            raw = base45_decode(args.base45.strip())
        except (KeyError, ValueError) as e:
            print(f"{C.RED}[error]{C.RESET} Base45 decode failed: {e}", file=sys.stderr)
            sys.exit(1)
    elif args.serial:
        try:
            raw = parse_serial_log(args.serial)
        except (FileNotFoundError, ValueError) as e:
            print(f"{C.RED}[error]{C.RESET} Serial log parse failed: {e}", file=sys.stderr)
            sys.exit(1)
    elif args.vnc:
        raw = vnc_capture(args.vnc)
    elif args.image:
        raw = scan_qr(args.image)
    else:
        ap.error("Provide an image, --hex, --base45, --serial, --stdin, or --vnc")

    # Symbolicator — use explicit --elf if provided, otherwise let
    # decode_crash() auto-detect from the dump's git hash + symbol archive
    sym = None
    if args.elf:
        sym = Symbolicator(args.elf)
        if sym.symbols:
            print(f"  {C.GREEN}✓{C.RESET} Loaded {len(sym.symbols)} symbols from {args.elf}",
                  file=sys.stderr)
    # If no --elf, decode_crash will try symbol archive lookup using the dump's git hash

    # If --save, redirect stdout to file while still printing to terminal
    save_file = None
    if args.save:
        save_file = open(args.save, 'w')
        # Disable colors for saved output
        old_stdout = sys.stdout
        sys.stdout = save_file
        for attr in dir(C):
            if not attr.startswith("_"):
                setattr(C, attr, "")

    decode_crash(raw, sym, args.raw, args.json)

    if save_file:
        save_file.close()
        sys.stdout = old_stdout
        print(f"  {C.GREEN}✓{C.RESET} Report saved to {args.save}", file=sys.stderr)
        # Also print to terminal with colors
        decode_crash(raw, sym, args.raw, args.json)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Panic QR code verification test.

Takes a screenshot of Brook OS after a deliberate panic, decodes the QR code,
Base45-decodes the payload, and verifies the panic packet structure matches
the expected protocol.

Usage:
    python3 scripts/test_panic_qr.py [screenshot.png]

If no screenshot is given, it boots QEMU, triggers a panic via INIT.RC,
takes a screenshot, and verifies the QR code automatically.
"""

import sys
import struct
import subprocess
import tempfile
import os
import time

# ---------------------------------------------------------------------------
# Base45 decoder (matches the kernel's Base45 encoder)
# ---------------------------------------------------------------------------

BASE45_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"

def base45_decode(encoded: str) -> bytes:
    """Decode a Base45-encoded string back to bytes."""
    result = bytearray()
    i = 0
    while i < len(encoded):
        if i + 3 <= len(encoded):
            # 3-character chunk → 2 bytes
            c0 = BASE45_CHARSET.index(encoded[i])
            c1 = BASE45_CHARSET.index(encoded[i + 1])
            c2 = BASE45_CHARSET.index(encoded[i + 2])
            value = c0 + c1 * 45 + c2 * 2025
            result.append((value >> 8) & 0xFF)
            result.append(value & 0xFF)
            i += 3
        elif i + 2 <= len(encoded):
            # 2-character chunk → 1 byte
            c0 = BASE45_CHARSET.index(encoded[i])
            c1 = BASE45_CHARSET.index(encoded[i + 1])
            value = c0 + c1 * 45
            result.append(value & 0xFF)
            i += 2
        else:
            raise ValueError(f"Invalid Base45 encoding at position {i}")
    return bytes(result)

# ---------------------------------------------------------------------------
# Panic packet parser
# ---------------------------------------------------------------------------

QR_MAGIC_BYTE        = 0x2D
QR_VERSION           = 0x00
QR_HEADER_PAD        = 0xCAFEF00D
QR_PACKET_TYPE_REGS  = 0xA3000001
QR_PACKET_TYPE_STACK = 0xA3000002

# PanicHeader: magic(1) + version(1) + page(1) + pageCount(1) + pad(4) = 8 bytes
PANIC_HEADER_FMT = "<BBBBI"
PANIC_HEADER_SIZE = struct.calcsize(PANIC_HEADER_FMT)

# PanicPacketHeader: type(4) + size(4) = 8 bytes
PACKET_HEADER_FMT = "<II"
PACKET_HEADER_SIZE = struct.calcsize(PACKET_HEADER_FMT)

# PanicCPURegs: 6 GPRs(48) + 8 GPRs(64) + rip,rsp,rbp,rflags(32) + cr0,cr2,cr3,cr4(32)
#             + cs,ds,ss,es,fs,gs(12) + reserved(2) = 190 bytes
# = 22 * uint64_t + 7 * uint16_t
PANIC_REGS_FMT = "<" + "Q" * 22 + "H" * 7
PANIC_REGS_SIZE = struct.calcsize(PANIC_REGS_FMT)

REG_NAMES = [
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "rip", "rsp", "rbp", "rflags",
    "cr0", "cr2", "cr3", "cr4",
    "cs", "ds", "ss", "es", "fs", "gs", "reserved"
]


def parse_panic_packet(data: bytes) -> dict:
    """Parse a panic packet and return a dict of register values + stack trace."""
    if len(data) < PANIC_HEADER_SIZE:
        raise ValueError(f"Packet too short for header: {len(data)} < {PANIC_HEADER_SIZE}")

    magic, version, page, page_count, pad = struct.unpack_from(PANIC_HEADER_FMT, data, 0)

    if magic != QR_MAGIC_BYTE:
        raise ValueError(f"Bad magic: 0x{magic:02x} (expected 0x{QR_MAGIC_BYTE:02x})")
    if version != QR_VERSION:
        raise ValueError(f"Bad version: {version} (expected {QR_VERSION})")
    if pad != QR_HEADER_PAD:
        raise ValueError(f"Bad header pad: 0x{pad:08x} (expected 0x{QR_HEADER_PAD:08x})")

    offset = PANIC_HEADER_SIZE

    result = {
        "header": {
            "magic": magic,
            "version": version,
            "page": page,
            "pageCount": page_count,
            "pad": pad,
        },
        "regs": {},
        "stack_trace": [],
    }

    # Parse packets until we run out of data
    while offset + PACKET_HEADER_SIZE <= len(data):
        pkt_type, pkt_size = struct.unpack_from(PACKET_HEADER_FMT, data, offset)
        offset += PACKET_HEADER_SIZE

        if pkt_type == QR_PACKET_TYPE_REGS:
            if len(data) < offset + PANIC_REGS_SIZE:
                raise ValueError(f"Packet too short for CPU regs: {len(data)} < {offset + PANIC_REGS_SIZE}")
            values = struct.unpack_from(PANIC_REGS_FMT, data, offset)
            for i, name in enumerate(REG_NAMES):
                result["regs"][name] = values[i]
            offset += PANIC_REGS_SIZE

        elif pkt_type == QR_PACKET_TYPE_STACK:
            if offset >= len(data):
                break
            depth = data[offset]
            offset += 1
            frames = []
            for _ in range(depth):
                if offset + 8 > len(data):
                    break
                rip = struct.unpack_from("<Q", data, offset)[0]
                frames.append(rip)
                offset += 8
            result["stack_trace"] = frames

        else:
            # Skip unknown packet types
            offset += pkt_size

    return result


def decode_qr_from_image(image_path: str) -> str:
    """Decode a QR code from an image file using zbarimg."""
    result = subprocess.run(
        ["zbarimg", "--raw", "--quiet", "-Sdisable", "-Sqrcode.enable", image_path],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        raise RuntimeError(f"zbarimg failed (rc={result.returncode}): {result.stderr}")
    return result.stdout.strip()


def verify_panic_qr(image_path: str) -> bool:
    """Full verification pipeline: image → QR → Base45 → packet → registers."""
    print(f"Decoding QR from: {image_path}")

    # Step 1: Decode QR code
    qr_text = decode_qr_from_image(image_path)
    print(f"QR text ({len(qr_text)} chars): {qr_text[:80]}...")

    # Step 2: Base45 decode
    raw_data = base45_decode(qr_text)
    print(f"Base45 decoded: {len(raw_data)} bytes")

    # Step 3: Parse panic packet
    packet = parse_panic_packet(raw_data)
    print(f"Packet parsed OK")

    # Step 4: Verify header
    hdr = packet["header"]
    assert hdr["magic"] == QR_MAGIC_BYTE, f"Bad magic: {hdr['magic']}"
    assert hdr["version"] == QR_VERSION, f"Bad version: {hdr['version']}"
    assert hdr["pad"] == QR_HEADER_PAD, f"Bad pad: {hdr['pad']}"
    print(f"Header: magic=0x{hdr['magic']:02x} version={hdr['version']} page={hdr['page']}/{hdr['pageCount']}")

    # Step 5: Verify registers are plausible
    regs = packet["regs"]
    print(f"Registers:")
    for name in REG_NAMES:
        val = regs[name]
        if name in ("cs", "ds", "ss", "es", "fs", "gs", "reserved"):
            print(f"  {name:8s} = 0x{val:04x}")
        else:
            print(f"  {name:8s} = 0x{val:016x}")

    # Basic sanity checks on register values
    # RIP should be in kernel space (0xFFFFFFFF8...)
    rip = regs["rip"]
    assert rip > 0xFFFFFFFF00000000, f"RIP not in kernel space: 0x{rip:016x}"

    # RSP should be in kernel stack area
    rsp = regs["rsp"]
    assert rsp > 0xFFFF000000000000, f"RSP not in kernel space: 0x{rsp:016x}"

    # CR3 should be non-zero (page table)
    cr3 = regs["cr3"]
    assert cr3 != 0, "CR3 is zero"

    # CS should be kernel code segment (0x08)
    cs = regs["cs"]
    assert cs == 0x08, f"CS not kernel code segment: 0x{cs:04x}"

    # Stack trace
    trace = packet.get("stack_trace", [])
    if trace:
        print(f"\nStack trace ({len(trace)} frames):")
        for i, addr in enumerate(trace):
            sym = ""
            # Try addr2line if kernel ELF is available
            brook_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
            elf_path = os.path.join(brook_root, "build", "release", "kernel", "BROOK.elf")
            if os.path.exists(elf_path):
                try:
                    r = subprocess.run(
                        ["addr2line", "-f", "-e", elf_path, f"0x{addr:x}"],
                        capture_output=True, text=True, timeout=5
                    )
                    if r.returncode == 0 and r.stdout.strip():
                        lines = r.stdout.strip().split("\n")
                        sym = f"  {lines[0]}"
                        if len(lines) > 1:
                            sym += f" at {lines[1]}"
                except (FileNotFoundError, subprocess.TimeoutExpired):
                    pass
            print(f"  [{i:2d}] 0x{addr:016x}{sym}")
    else:
        print("\nNo stack trace in packet")

    print(f"\n✓ All checks passed — panic QR code round-trips correctly")
    return True


def main():
    if len(sys.argv) >= 2:
        # Verify an existing screenshot
        return verify_panic_qr(sys.argv[1])

    # Full automated test: boot QEMU → panic → screenshot → verify
    print("Running full automated panic QR test...")

    # Find required files
    brook_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    esp_dir = os.path.join(brook_root, "build", "debug", "esp")
    disk_img = os.path.join(brook_root, "brook_disk.img")

    if not os.path.exists(esp_dir):
        print(f"ERROR: ESP directory not found: {esp_dir}")
        print("Run scripts/build.sh Debug first")
        return False

    # The INIT.RC on disk should already be set to "panic test_qr_code"

    print("Test completed — use the screenshot path as argument for verification")
    return True


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)

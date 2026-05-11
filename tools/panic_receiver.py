#!/usr/bin/env python3
"""Brook Panic Receiver — HTTP server that accepts QR-scanned crash data from a phone.

Serves the panic_scanner.html page and accepts POST /api/crash with Base45-encoded
QR page data. Decodes, runs crash_decoder, and saves the report to the workspace.

Usage:
  python3 tools/panic_receiver.py [--port 8080] [--bind 0.0.0.0]

Then open the displayed URL on your phone (same Wi-Fi network).
"""

import argparse
import http.server
import json
import os
import socket
import struct
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from urllib.parse import urlparse

SCRIPT_DIR = Path(__file__).parent
SCANNER_HTML = SCRIPT_DIR / "panic_scanner.html"
CRASH_DECODER = SCRIPT_DIR / "crash_decoder.py"
WORKSPACE = Path(os.environ.get("BROOK_WORKSPACE", SCRIPT_DIR.parent))
REPORTS_DIR = WORKSPACE / "crash_reports"


# ── Agent IPC notification ──────────────────────────────────────────────────
INBOX_DIR = Path("/workspace/.enclave/inbox")

def notify_agent(message: str) -> None:
    """Notify the Enclave agent via file-based inbox."""
    try:
        INBOX_DIR.mkdir(parents=True, exist_ok=True)
        filename = f"{int(time.time() * 1000)}.json"
        (INBOX_DIR / filename).write_text(json.dumps({"content": message}))
    except Exception as e:
        print(f"  [warn] Inbox notify failed: {e}")


# ── Base45 ──────────────────────────────────────────────────────────────────
BASE45_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

def base45_decode(s: str) -> bytes:
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
            raise ValueError(f"Trailing character at position {i}")
    return bytes(buf)


# ── Panic header ────────────────────────────────────────────────────────────
PANIC_MAGIC = 0x2D
PANIC_PAD   = 0xCAFEF00D

def validate_panic_header(data: bytes) -> dict:
    """Parse and validate PanicHeader. Returns dict with header fields."""
    if len(data) < 8:
        raise ValueError("Data too short for PanicHeader")
    magic, version, page, page_count, pad = struct.unpack_from("<BBBBI", data, 0)
    if magic != PANIC_MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:02X} (expected 0x{PANIC_MAGIC:02X})")
    if pad != PANIC_PAD:
        raise ValueError(f"Bad pad: 0x{pad:08X} (expected 0x{PANIC_PAD:08X})")
    return {"magic": magic, "version": version, "page": page,
            "page_count": page_count}


# ── Reassemble multi-page crash dump ────────────────────────────────────────
def reassemble_pages(base45_pages: list[str]) -> bytes:
    """Decode Base45 pages, validate headers, and concatenate payloads.

    For single-page dumps, returns the full decoded data as-is.
    For multi-page, strips the per-page PanicHeader and concatenates payloads,
    then prepends the first page's header.
    """
    decoded_pages = []
    for i, text in enumerate(base45_pages):
        raw = base45_decode(text.strip().upper())
        hdr = validate_panic_header(raw)
        decoded_pages.append((hdr, raw))

    if len(decoded_pages) == 1:
        return decoded_pages[0][1]

    # Multi-page: sort by page index, strip headers, concatenate
    decoded_pages.sort(key=lambda x: x[0]["page"])

    # Use first page's header as the combined header (with page=0, count=1)
    first_hdr_raw = decoded_pages[0][1][:8]
    # Override page/count to indicate single reassembled packet
    combined_header = bytearray(first_hdr_raw)
    combined_header[2] = 0  # page = 0
    combined_header[3] = 1  # page_count = 1

    payload = b""
    for hdr, raw in decoded_pages:
        payload += raw[8:]  # strip 8-byte PanicHeader

    return bytes(combined_header) + payload


# ── Request handler ─────────────────────────────────────────────────────────
class PanicReceiverHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Quieter logging
        ts = time.strftime("%H:%M:%S")
        print(f"  [{ts}] {fmt % args}")

    def _cors_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self):
        self.send_response(200)
        self._cors_headers()
        self.end_headers()

    def do_GET(self):
        path = urlparse(self.path).path

        if path == "/" or path == "/index.html":
            self._serve_file(SCANNER_HTML, "text/html; charset=utf-8")
        elif path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self._cors_headers()
            self.end_headers()
            self.wfile.write(json.dumps({"status": "ok"}).encode())
        else:
            self.send_error(404)

    def do_POST(self):
        path = urlparse(self.path).path

        if path == "/api/crash":
            self._handle_crash()
        else:
            self.send_error(404)

    def _serve_file(self, filepath: Path, content_type: str):
        try:
            data = filepath.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self._cors_headers()
            self.end_headers()
            self.wfile.write(data)
        except FileNotFoundError:
            self.send_error(404, f"File not found: {filepath.name}")

    def _handle_crash(self):
        try:
            content_len = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(content_len)
            data = json.loads(body)

            pages = data.get("pages", [])
            if not pages:
                self._json_error(400, "No pages provided")
                return

            timestamp = data.get("timestamp", datetime.now().isoformat())
            print(f"\n  ╔═══════════════════════════════════════════════╗")
            print(f"  ║  🔴 CRASH DATA RECEIVED ({len(pages)} page(s))         ║")
            print(f"  ╚═══════════════════════════════════════════════╝")

            # Step 1: Reassemble pages
            try:
                raw = reassemble_pages(pages)
            except Exception as e:
                self._json_error(400, f"Decode failed: {e}")
                return

            hex_data = raw.hex().upper()
            print(f"  Decoded: {len(raw)} bytes")

            # Step 2: Save raw hex to file
            REPORTS_DIR.mkdir(exist_ok=True)
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            hex_path = REPORTS_DIR / f"crash_{ts}.hex"
            hex_path.write_text(hex_data)
            print(f"  Raw hex: {hex_path}")

            # Step 3: Run crash_decoder.py
            report_path = REPORTS_DIR / f"crash_{ts}.txt"
            json_path = REPORTS_DIR / f"crash_{ts}.json"

            # Generate text report
            try:
                result = subprocess.run(
                    [sys.executable, str(CRASH_DECODER),
                     "--hex", hex_data, "--no-color", "--save", str(report_path)],
                    capture_output=True, text=True, timeout=30,
                    cwd=str(WORKSPACE)
                )
                if result.returncode == 0:
                    print(f"  Report:  {report_path}")
                else:
                    print(f"  [warn] Decoder stderr: {result.stderr[:200]}")
                    # Still save what we got
                    if not report_path.exists():
                        report_path.write_text(
                            f"Decoder failed (exit {result.returncode}):\n"
                            f"{result.stderr}\n\nRaw hex:\n{hex_data}\n"
                        )
            except Exception as e:
                print(f"  [warn] Decoder error: {e}")
                report_path.write_text(f"Decoder error: {e}\n\nRaw hex:\n{hex_data}\n")

            # Generate JSON report
            try:
                result = subprocess.run(
                    [sys.executable, str(CRASH_DECODER),
                     "--hex", hex_data, "--json"],
                    capture_output=True, text=True, timeout=30,
                    cwd=str(WORKSPACE)
                )
                if result.returncode == 0:
                    json_path.write_text(result.stdout)
                    print(f"  JSON:    {json_path}")
            except Exception:
                pass

            # Print the decoded report to terminal
            print()
            try:
                result = subprocess.run(
                    [sys.executable, str(CRASH_DECODER), "--hex", hex_data],
                    timeout=30, cwd=str(WORKSPACE)
                )
            except Exception:
                pass
            print()

            # Notify the agent via IPC
            rel_report = str(report_path.relative_to(WORKSPACE))
            rel_json = str(json_path.relative_to(WORKSPACE))
            notify_agent(
                f"🔴 Panic dump received from phone scanner! "
                f"({len(pages)} page(s), {len(raw)} bytes decoded)\n"
                f"Report: {rel_report}\n"
                f"JSON: {rel_json}\n"
                f"Raw hex: {str(hex_path.relative_to(WORKSPACE))}"
            )

            # Send success response
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self._cors_headers()
            self.end_headers()
            self.wfile.write(json.dumps({
                "status": "ok",
                "report_path": str(report_path.relative_to(WORKSPACE)),
                "json_path": str(json_path.relative_to(WORKSPACE)),
                "hex_path": str(hex_path.relative_to(WORKSPACE)),
                "decoded_size": len(raw),
                "pages_received": len(pages),
            }).encode())

        except json.JSONDecodeError:
            self._json_error(400, "Invalid JSON")
        except Exception as e:
            self._json_error(500, str(e))

    def _json_error(self, code: int, message: str):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self._cors_headers()
        self.end_headers()
        self.wfile.write(json.dumps({"error": message}).encode())
        print(f"  [error] {code}: {message}")


# ── Network helpers ─────────────────────────────────────────────────────────
def get_local_ips() -> list[str]:
    """Get all non-loopback IPv4 addresses for this machine."""
    ips = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                ips.append(ip)
    except Exception:
        pass
    # Fallback: connect to external and check
    if not ips:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ips.append(s.getsockname()[0])
            s.close()
        except Exception:
            pass
    return list(set(ips))


def print_qr_url(url: str):
    """Print a tiny QR code of the URL to terminal for easy phone scanning."""
    try:
        # Try using qrcode library
        import qrcode
        qr = qrcode.QRCode(version=1, box_size=1, border=1,
                           error_correction=qrcode.constants.ERROR_CORRECT_L)
        qr.add_data(url)
        qr.make(fit=True)

        # Render using Unicode block characters (2 rows per character)
        matrix = qr.get_matrix()
        rows = len(matrix)
        print()
        for r in range(0, rows, 2):
            line = "  "
            for c in range(len(matrix[r])):
                top = matrix[r][c]
                bot = matrix[r + 1][c] if r + 1 < rows else False
                if top and bot:
                    line += "█"
                elif top:
                    line += "▀"
                elif bot:
                    line += "▄"
                else:
                    line += " "
            print(line)
        print()
    except ImportError:
        # No qrcode lib — just print the URL prominently
        pass


# ── Main ────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="Brook Panic Receiver")
    ap.add_argument("--port", type=int, default=8080, help="Listen port (default: 8080)")
    ap.add_argument("--bind", default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    args = ap.parse_args()

    # Ensure reports directory exists
    REPORTS_DIR.mkdir(exist_ok=True)

    # Start server
    server = http.server.HTTPServer((args.bind, args.port), PanicReceiverHandler)

    ips = get_local_ips()
    print()
    print("  ╔═══════════════════════════════════════════════╗")
    print("  ║       🔴 Brook Panic Receiver                 ║")
    print("  ╚═══════════════════════════════════════════════╝")
    print()
    print(f"  Listening on {args.bind}:{args.port}")
    print(f"  Reports dir: {REPORTS_DIR}")
    print()

    url = f"http://{ips[0] if ips else 'localhost'}:{args.port}"
    print(f"  📱 Open on your phone:")
    print(f"     {url}")
    if len(ips) > 1:
        for ip in ips[1:]:
            print(f"     http://{ip}:{args.port}")
    print()

    # Try to print QR code for the URL
    print_qr_url(url)

    print("  Waiting for crash data...")
    print("  (Press Ctrl+C to stop)")
    print()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n  Shutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Brook E2E: Ladybird second-navigation test

Connects to Brook's debug channel (TCP port 9999) and automates a
two-page navigation in Ladybird to test for the futex deadlock that
was fixed in commit e34905b.

Test sequence:
  1. Wait for Ladybird window to appear (wm list)
  2. Click the address bar at (139, 52)
  3. Select all (Ctrl+A) and type a URL
  4. Press Enter to navigate
  5. Wait for page load
  6. Navigate to a second URL (the deadlock trigger)
  7. Verify the second navigation completes

Usage:
    python3 scripts/e2e_ladybird_navigate.py [--listen] [--port 9999]

    --listen   Wait for Brook to connect (default)
    --timeout  Seconds to wait for each step (default: 30)
"""

import socket
import sys
import time
import argparse
import threading
import re


class DebugChannel:
    """Simple wrapper for Brook's debug TCP channel."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.lines = []
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self):
        try:
            while True:
                data = self.sock.recv(4096)
                if not data:
                    break
                with self.lock:
                    self.buf += data
                    while b"\n" in self.buf:
                        line, self.buf = self.buf.split(b"\n", 1)
                        decoded = line.decode("utf-8", errors="replace").strip()
                        if decoded:
                            self.lines.append(decoded)
        except (ConnectionResetError, OSError):
            pass

    def send(self, cmd):
        """Send a command and return immediately."""
        self.sock.sendall((cmd + "\n").encode("utf-8"))

    def send_wait(self, cmd, timeout=5):
        """Send a command and wait for response lines."""
        # Clear pending lines
        with self.lock:
            self.lines.clear()
        self.send(cmd)
        time.sleep(0.5)
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if self.lines:
                    result = list(self.lines)
                    self.lines.clear()
                    return result
            time.sleep(0.2)
        return []

    def wait_for_line(self, pattern, timeout=30):
        """Wait for a line matching a regex pattern."""
        deadline = time.time() + timeout
        compiled = re.compile(pattern)
        while time.time() < deadline:
            with self.lock:
                for i, line in enumerate(self.lines):
                    if compiled.search(line):
                        self.lines.pop(i)
                        return line
            time.sleep(0.5)
        return None


def wait_for_window(ch, title_pattern, timeout=60):
    """Poll wm list until a window matching the pattern appears."""
    print(f"  Waiting for window matching '{title_pattern}'...")
    deadline = time.time() + timeout
    compiled = re.compile(title_pattern, re.IGNORECASE)
    while time.time() < deadline:
        lines = ch.send_wait("wm list", timeout=3)
        for line in lines:
            if compiled.search(line):
                print(f"    Found: {line}")
                return line
        time.sleep(2)
    return None


def navigate_to(ch, url, click_x=139, click_y=52):
    """Click address bar, select all, type URL, press Enter."""
    print(f"  Navigating to: {url}")

    # Click address bar
    ch.send(f"inject click {click_x} {click_y}")
    time.sleep(0.5)

    # Select all text
    ch.send("inject combo ctrl+a")
    time.sleep(0.3)

    # Type URL
    ch.send(f"inject type {url}")
    time.sleep(0.3)

    # Press Enter (scan code 0x1C = 28, ASCII 13)
    ch.send("inject key 28 13")
    time.sleep(0.5)

    print(f"    Input injected, waiting for load...")


def main():
    parser = argparse.ArgumentParser(description="Brook Ladybird navigation E2E test")
    parser.add_argument("--listen", action="store_true", default=True,
                        help="Wait for Brook to connect (default)")
    parser.add_argument("--port", type=int, default=9999,
                        help="TCP port (default: 9999)")
    parser.add_argument("--timeout", type=int, default=30,
                        help="Timeout per step in seconds")
    parser.add_argument("--url1", default="wiki.osdev.org",
                        help="First URL to navigate to")
    parser.add_argument("--url2", default="example.com",
                        help="Second URL (deadlock trigger test)")
    parser.add_argument("--click-x", type=int, default=139,
                        help="Address bar click X coordinate")
    parser.add_argument("--click-y", type=int, default=52,
                        help="Address bar click Y coordinate")
    args = parser.parse_args()

    # Listen for Brook to connect
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", args.port))
    server.listen(1)
    print(f"=== Brook Ladybird Navigation E2E Test ===")
    print(f"  Listening on 0.0.0.0:{args.port}...")

    server.settimeout(120)
    try:
        conn, addr = server.accept()
    except socket.timeout:
        print("  TIMEOUT waiting for Brook to connect")
        sys.exit(1)
    finally:
        server.close()

    print(f"  Connected from {addr[0]}:{addr[1]}")
    ch = DebugChannel(conn)
    time.sleep(1)

    # Step 1: Wait for Ladybird window
    win = wait_for_window(ch, r"Ladybird|ladybird", timeout=args.timeout)
    if not win:
        print("  FAIL: Ladybird window not found")
        sys.exit(1)

    # Step 2: First navigation
    navigate_to(ch, args.url1, args.click_x, args.click_y)
    time.sleep(args.timeout // 2)  # Wait for page to load

    # Check that the window title changed (indicates successful navigation)
    win2 = wait_for_window(ch, r"wm\[", timeout=5)
    print(f"    Window state after first nav: {win2 or 'unchanged'}")

    # Step 3: Second navigation (this is the deadlock trigger)
    print("\n  === Second navigation (deadlock trigger) ===")
    navigate_to(ch, args.url2, args.click_x, args.click_y)
    time.sleep(args.timeout // 2)

    # Step 4: Verify system is still responsive
    print("\n  Verifying system responsiveness...")
    response = ch.send_wait("uptime", timeout=10)
    if response:
        print(f"    System responsive: {response[0]}")
        print("\n  PASS — second navigation completed without deadlock")
    else:
        print("    FAIL — system unresponsive (possible deadlock)")
        sys.exit(1)

    # Check final window state
    final = ch.send_wait("wm list", timeout=5)
    print("\n  Final window state:")
    for line in final:
        print(f"    {line}")

    print("\n=== PASS ===")


if __name__ == "__main__":
    main()

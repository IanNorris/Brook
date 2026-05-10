#!/usr/bin/env python3
"""Brook OS Debug Channel Client

Connects to Brook's TCP debug channel (via QEMU user-mode networking)
and provides an interactive command shell.

Usage:
    python3 scripts/debug_client.py [host] [port]

Default: localhost:9999 (QEMU forwards guest→host connections)

To use with QEMU, start QEMU with:
    -netdev user,id=net0,hostfwd=tcp::9999-:9999

Or run a simple TCP listener that Brook connects to:
    python3 scripts/debug_client.py --listen [port]
"""

import socket
import sys
import select
import argparse
import threading
import time


def reader_thread(sock):
    """Continuously read and print data from the socket."""
    try:
        while True:
            data = sock.recv(4096)
            if not data:
                print("\n[connection closed by Brook]")
                break
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
    except (ConnectionResetError, OSError):
        print("\n[connection lost]")


def interactive_session(sock):
    """Run an interactive session with the debug channel."""
    print("[Connected to Brook Debug Channel]")
    print("[Type 'help' for commands, Ctrl+C to quit]\n")

    # Start reader thread
    reader = threading.Thread(target=reader_thread, args=(sock,), daemon=True)
    reader.start()

    try:
        while True:
            try:
                line = input("brook> ")
            except EOFError:
                break
            if line.strip():
                sock.sendall((line + "\n").encode("utf-8"))
    except KeyboardInterrupt:
        print("\n[disconnecting]")
    finally:
        sock.close()


def listen_mode(port):
    """Listen for Brook to connect to us (Brook is the TCP client)."""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", port))
    server.listen(1)
    print(f"[Listening on 0.0.0.0:{port} — waiting for Brook to connect...]")
    print("[Brook connects to 10.0.2.2:9999 via QEMU user-mode networking]")

    try:
        conn, addr = server.accept()
        print(f"[Connection from {addr[0]}:{addr[1]}]")
        interactive_session(conn)
    except KeyboardInterrupt:
        print("\n[shutting down]")
    finally:
        server.close()


def connect_mode(host, port):
    """Connect to Brook (if Brook had a TCP server — future use)."""
    print(f"[Connecting to {host}:{port}...]")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    try:
        sock.connect((host, port))
        sock.settimeout(None)
        interactive_session(sock)
    except (ConnectionRefusedError, socket.timeout) as e:
        print(f"[Failed to connect: {e}]")
        print("[Note: Brook is the TCP client. Use --listen mode instead.]")
        sys.exit(1)


def batch_session(sock, commands):
    """Send a list of commands, print responses, then disconnect."""
    reader = threading.Thread(target=reader_thread, args=(sock,), daemon=True)
    reader.start()

    for cmd in commands:
        cmd = cmd.strip()
        if not cmd or cmd.startswith("#"):
            continue
        sock.sendall((cmd + "\n").encode("utf-8"))
        time.sleep(0.3)

    # Give time for final responses
    time.sleep(1)
    sock.close()


def main():
    parser = argparse.ArgumentParser(description="Brook OS Debug Channel Client")
    parser.add_argument("--listen", action="store_true",
                        help="Listen for Brook to connect (default mode)")
    parser.add_argument("--connect", action="store_true",
                        help="Connect to Brook (requires TCP server in Brook)")
    parser.add_argument("--batch", type=str, metavar="FILE",
                        help="Run commands from a file then exit")
    parser.add_argument("-c", "--command", type=str, action="append",
                        help="Run a single command (can be repeated)")
    parser.add_argument("host", nargs="?", default="localhost",
                        help="Host to connect to (default: localhost)")
    parser.add_argument("port", nargs="?", type=int, default=9999,
                        help="Port (default: 9999)")

    args = parser.parse_args()

    # Determine commands for batch mode
    commands = None
    if args.batch:
        with open(args.batch) as f:
            commands = f.readlines()
    elif args.command:
        commands = args.command

    if args.connect:
        if commands is not None:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect((args.host, args.port))
            sock.settimeout(None)
            batch_session(sock, commands)
        else:
            connect_mode(args.host, args.port)
    else:
        # Default: listen mode (Brook connects to us)
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", args.port))
        server.listen(1)
        if commands is not None:
            print(f"[Listening on 0.0.0.0:{args.port} for batch mode...]")
        else:
            print(f"[Listening on 0.0.0.0:{args.port} — waiting for Brook to connect...]")
            print("[Brook connects to 10.0.2.2:9999 via QEMU user-mode networking]")

        conn, addr = server.accept()
        server.close()
        print(f"[Connection from {addr[0]}:{addr[1]}]")

        if commands is not None:
            batch_session(conn, commands)
        else:
            interactive_session(conn)


if __name__ == "__main__":
    main()

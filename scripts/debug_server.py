#!/usr/bin/env python3
"""Brook OS TCP debug server.

Run this on the host before booting Brook. The kernel connects to port 9999
on boot and streams debug messages. You can type commands to query state.

Commands:
  mouse   - dump mouse/input state
  procs   - list running processes
  net     - network stack stats
  mem     - memory stats
  sock    - socket table
  log on  - enable real-time serial mirror
  log off - disable serial mirror
  <text>  - echo to kernel serial log
"""

import socket
import sys
import select
import threading

PORT = 9999

def reader_thread(conn):
    """Read and print data from Brook."""
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                print("\n[debug] Connection closed by Brook")
                break
            text = data.decode('utf-8', errors='replace')
            sys.stdout.write(text)
            sys.stdout.flush()
    except (ConnectionResetError, BrokenPipeError, OSError):
        print("\n[debug] Connection lost")

def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', PORT))
    srv.listen(1)
    print(f"[debug] Listening on port {PORT}... (boot Brook now)")

    conn, addr = srv.accept()
    print(f"[debug] Connected from {addr}")

    # Start reader thread
    t = threading.Thread(target=reader_thread, args=(conn,), daemon=True)
    t.start()

    # Main thread: read stdin and send commands
    try:
        while True:
            line = input()
            if not line:
                continue
            conn.sendall((line + '\n').encode('utf-8'))
    except (EOFError, KeyboardInterrupt):
        print("\n[debug] Shutting down")
    except (BrokenPipeError, OSError):
        print("\n[debug] Connection lost")
    finally:
        conn.close()
        srv.close()

if __name__ == '__main__':
    main()

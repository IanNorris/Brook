"""Serial-log tail with sentinel matching for the P2 gate.

The gate never sleeps for a fixed duration to "wait for readiness"; it waits
for a specific serial marker (sentinel) with a hard timeout. A sentinel
timeout is always a hard failure — never a fall-through to "act anyway",
because a hung app that never prints its marker must fail the gate, not pass
it on a stale frame.
"""
import re
import threading
import time


class SerialTail:
    def __init__(self, path: str):
        self.path = path
        self._lines = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def _run(self):
        # Follow the file as it grows, tolerating it not existing yet.
        f = None
        while not self._stop.is_set():
            if f is None:
                try:
                    f = open(self.path, "r", errors="replace")
                except OSError:
                    time.sleep(0.05)
                    continue
            line = f.readline()
            if line == "":
                time.sleep(0.02)
                continue
            with self._lock:
                self._lines.append(line.rstrip("\n"))

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)

    def snapshot(self):
        with self._lock:
            return list(self._lines)

    def count_matches(self, pattern):
        rx = re.compile(pattern)
        with self._lock:
            return sum(1 for ln in self._lines if rx.search(ln))

    def wait_for(self, pattern, timeout, start_index=0):
        """Block until a line at or after start_index matches `pattern`.

        Returns (matched_line, next_index) so a caller can chain waits without
        re-matching earlier lines. Raises TimeoutError on timeout (hard fail).
        """
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        idx = start_index
        while time.time() < deadline:
            with self._lock:
                n = len(self._lines)
                while idx < n:
                    ln = self._lines[idx]
                    idx += 1
                    if rx.search(ln):
                        return ln, idx
            time.sleep(0.03)
        raise TimeoutError(f"sentinel timeout after {timeout}s waiting for /{pattern}/")

    def current_index(self):
        with self._lock:
            return len(self._lines)

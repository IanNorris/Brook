"""Agent-presence heartbeats behind the guest taskbar agent indicator.

Only the long-lived MCP stdio server runs this. One-shot CLI calls never
attach presence (their gaps would look like a dead agent), although their
framed requests refresh a presence the server already holds.

The guest sees only its debug listener. It shows the agent icon while beats
are fresh, a warning variant once they have been silent for 3x the interval
(minimum 30 s), and nothing after an explicit detach or 10 minutes of
silence. A clean server exit sends detach so the guest can tell a deliberate
disconnect from a lost one; a killed server and a host/network outage remain
indistinguishable to the guest.

Beats deliberately do not take the per-session operation lock. They are one
short idempotent framed line on their own connection, so they may interleave
between an operation's connections (as any separate client can) and keep the
indicator fresh during long transfers. BROOK_BRIDGE_HEARTBEAT=0 disables them
(for example when measuring idle network behaviour).
"""
import os
import threading
import time

INTERVAL_S = 10
# Stop (and detach) a session the agent has not used for this long, so an
# abandoned chat's server does not claim a connected agent indefinitely.
IDLE_DETACH_S = 30 * 60
BEAT_TIMEOUT_S = 3


class PresenceGone(Exception):
    """The session can no longer be heartbeated (stopped, rebooted, detached, unsupported)."""


def enabled():
    return os.environ.get('BROOK_BRIDGE_HEARTBEAT', '1') != '0'


class Presence:
    def __init__(self, send, interval=INTERVAL_S, idle_detach=None, clock=time.monotonic):
        # send(kind, session, command) -> reply dict. It raises PresenceGone
        # for permanent conditions and OSError for transient transport loss.
        self.send = send
        self.interval = interval
        self.idle_detach = idle_detach if idle_detach is not None else float(
            os.environ.get('BROOK_BRIDGE_HEARTBEAT_IDLE_S', IDLE_DETACH_S))
        self.clock = clock
        self.targets = {}
        self.lock = threading.Lock()
        # Held across each send so forget() can wait out an in-flight beat
        # before the caller sends its own detach.
        self.send_lock = threading.Lock()
        self.wake = threading.Event()
        self.stopping = threading.Event()
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self.run, name='presence', daemon=True)
        self.thread.start()
        return self

    def touch(self, kind, session):
        if kind not in ('vm', 'hw') or not isinstance(session, str):
            return
        with self.lock:
            entry = self.targets.get((kind, session))
            fresh = entry is None
            if fresh:
                entry = self.targets[(kind, session)] = {'state': 'pending', 'failures': 0, 'last_beat': None}
            entry['last_use'] = self.clock()
        if fresh:
            self.wake.set()

    def forget(self, kind, session):
        with self.lock:
            removed = self.targets.pop((kind, session), None) is not None
        with self.send_lock:
            pass
        return removed

    def entry(self, kind, session):
        with self.lock:
            entry = self.targets.get((kind, session))
            return dict(entry, interval_s=self.interval) if entry else None

    def status(self):
        with self.lock:
            return {f'{k}:{s}': dict(v) for (k, s), v in self.targets.items()}

    def beat_one(self, kind, session, entry):
        now = self.clock()
        if now - entry['last_use'] >= self.idle_detach:
            self.forget(kind, session)
            self.detach(kind, session)
            return 'idle_detached'
        try:
            with self.send_lock:
                with self.lock:
                    if (kind, session) not in self.targets:
                        return 'forgotten'
                reply = self.send(kind, session, f'bridge presence beat {self.interval}')
        except PresenceGone:
            self.forget(kind, session)
            return 'gone'
        except OSError:
            with self.lock:
                entry['failures'] += 1
                entry['state'] = 'unreachable'
            return 'unreachable'
        if not isinstance(reply, dict) or reply.get('presence_api') != 1:
            # Older guest kernels: nothing to drive, stop sending.
            self.forget(kind, session)
            return 'unsupported'
        with self.lock:
            entry.update(state='beating', failures=0, last_beat=now)
        return 'beating'

    def tick(self):
        with self.lock:
            targets = list(self.targets.items())
        return {f'{k}:{s}': self.beat_one(k, s, entry) for (k, s), entry in targets}

    def detach(self, kind, session):
        try:
            self.send(kind, session, 'bridge presence detach')
            return True
        except (PresenceGone, OSError):
            return False

    def run(self):
        while not self.stopping.is_set():
            self.wake.clear()
            self.tick()
            # A newly touched session beats immediately; otherwise every interval.
            self.wake.wait(self.interval)

    def close(self):
        """Clean server exit: stop beating and tell each guest the agent left."""
        self.stopping.set()
        self.wake.set()
        if self.thread is not None:
            self.thread.join(timeout=BEAT_TIMEOUT_S + 1)
        with self.lock:
            targets = list(self.targets)
            self.targets.clear()
        for kind, session in targets:
            self.detach(kind, session)

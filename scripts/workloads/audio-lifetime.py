#!/usr/bin/env python3
"""Bounded guest-only /dev/dsp lifetime test; capture PCM through the VM bridge.

Run idle mode to check silence while an fd stays open and subsequent restart.
Kill mode owns and reaps its child, avoiding external PID reuse races.
"""
import array
import fcntl
import math
import os
import signal
import sys
import time


def open_dsp():
    fd = os.open('/dev/dsp', os.O_WRONLY)
    for request, value in [(0xc0045005, 16), (0xc0045006, 2), (0xc0045002, 44100)]:
        fcntl.ioctl(fd, request, array.array('i', [value]), True)
    return fd


samples = array.array('h')
for i in range(4410):
    value = int(7000 * math.sin(i * 2 * math.pi * 440 / 44100))
    samples.extend([value, value])
chunk = samples.tobytes()


def tone(fd, chunks):
    for _ in range(chunks):
        remaining = chunk
        while remaining:
            count = os.write(fd, remaining)
            if count <= 0:
                raise RuntimeError('audio write made no progress')
            remaining = remaining[count:]


mode = sys.argv[1] if len(sys.argv) > 1 else 'idle'
if mode == 'idle':
    fd = open_dsp()
    print('AUDIO_TONE', flush=True)
    tone(fd, 30)
    print('AUDIO_IDLE_OPEN_FD', flush=True)
    time.sleep(12)
    print('AUDIO_RESTART', flush=True)
    tone(fd, 30)
    os.close(fd)
    print('AUDIO_CLOSED', flush=True)
elif mode == 'kill':
    child = os.fork()
    if child == 0:
        # Finite fallback even if the parent's test fails before sending SIGKILL.
        tone(open_dsp(), 100)
        os._exit(1)
    time.sleep(2)
    os.kill(child, signal.SIGKILL)
    reaped, status = os.waitpid(child, 0)
    if reaped != child or not os.WIFSIGNALED(status) or os.WTERMSIG(status) != signal.SIGKILL:
        raise RuntimeError('audio writer did not exit through SIGKILL')
    print('AUDIO_KILL_REAPED', flush=True)
    time.sleep(3)
    print('AUDIO_KILL_SETTLED', flush=True)
else:
    raise SystemExit('usage: audio-lifetime.py [idle|kill]')

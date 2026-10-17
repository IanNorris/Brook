#!/usr/bin/env python3
"""Compare ALSA playback delay with the same OSS descriptor's queued bytes.

Run only inside the test VM; opens its configured default output. A successful
run collects observations, not a claim of correct audio timing.
"""
import array
import ctypes as c
import fcntl
import json
import math
import os
import stat
import sys
import time


def main():
    library = sys.argv[1] if len(sys.argv) > 1 else "libasound.so.2"
    pcm_name = sys.argv[2] if len(sys.argv) > 2 else "default"
    rounds = int(os.environ.get("BROOK_ALSA_CLOCK_ROUNDS", "12"))
    if not 1 <= rounds <= 200:
        raise ValueError("BROOK_ALSA_CLOCK_ROUNDS must be between 1 and 200")
    alsa = c.CDLL(library)
    alsa.snd_pcm_open.argtypes = [c.POINTER(c.c_void_p), c.c_char_p, c.c_int, c.c_int]
    alsa.snd_pcm_set_params.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_uint,
                                      c.c_uint, c.c_int, c.c_uint]
    alsa.snd_pcm_writei.argtypes = [c.c_void_p, c.c_void_p, c.c_ulong]
    alsa.snd_pcm_writei.restype = c.c_long
    alsa.snd_pcm_delay.argtypes = [c.c_void_p, c.POINTER(c.c_long)]
    for name in ["snd_pcm_start", "snd_pcm_drop", "snd_pcm_close"]:
        getattr(alsa, name).argtypes = [c.c_void_p]

    def check(result, operation):
        if result < 0:
            raise RuntimeError(f"{operation}: ALSA error {result}")
        return result

    pcm = c.c_void_p()
    check(alsa.snd_pcm_open(c.byref(pcm), pcm_name.encode(), 0, 0), "open")
    try:
        # S16_LE, RW_INTERLEAVED, stereo, 44100 Hz, resampling, 200 ms latency.
        check(alsa.snd_pcm_set_params(pcm, 2, 3, 2, 44100, 1, 200000), "set_params")
        samples = array.array("h")
        for i in range(8192):
            value = round(2000 * math.sin(2 * math.pi * 440 * i / 44100))
            samples.extend([value, value])
        buffer = c.create_string_buffer(samples.tobytes())
        started = time.monotonic()
        written = check(alsa.snd_pcm_writei(pcm, buffer, 8192), "writei")
        # writei can auto-start; EBUSY/EBADFD for an already-running stream
        # is recorded rather than concealed as successful explicit start.
        start_result = alsa.snd_pcm_start(pcm)
        descriptors = []
        for fd in range(3, 256):
            try:
                info = os.fstat(fd)
            except OSError:
                continue
            if stat.S_ISCHR(info.st_mode) and info.st_rdev == 0x0E03:
                descriptors.append(fd)
        print("ALSA_CLOCK_SETUP", json.dumps({"pcm": pcm_name,
              "written_frames": written, "start_result": start_result,
              "oss_fds": descriptors}), flush=True)
        total_written = written
        for _ in range(rounds):
            # The file PCM buffers an initial write in userspace. Continue
            # feeding it so observations also cover real /dev/dsp traffic.
            write_started = time.monotonic()
            total_written += check(alsa.snd_pcm_writei(pcm, buffer, 2048), "writei")
            write_elapsed = time.monotonic() - write_started
            delay = c.c_long()
            result = alsa.snd_pcm_delay(pcm, c.byref(delay))
            queues = []
            for fd in descriptors:
                queued = array.array("i", [0])
                try:
                    fcntl.ioctl(fd, 0x80045017, queued, True)  # GETODELAY
                except OSError as error:
                    queues.append({"fd": fd, "errno": error.errno})
                else:
                    queues.append({"fd": fd, "bytes": queued[0]})
            print("ALSA_CLOCK_SAMPLE", json.dumps({"elapsed_s": time.monotonic() - started,
                  "written_frames": total_written, "write_elapsed_s": write_elapsed,
                  "delay_result": result, "alsa_delay_frames": delay.value,
                  "oss_queues": queues}), flush=True)
            time.sleep(0.025)
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)
    print("ALSA_CLOCK_COLLECTED", flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Accept a clocked ALSA OSS slave inside an isolated, silent Brook VM.

Pass libasound path, configured PCM name and clock/exact. Never changes the
default route. Exact mode uses the same known samples as dsp_short_tail.py.
"""
import array
import ctypes as c
import errno
import fcntl
import json
import os
import stat
import sys
import time


class ChannelArea(c.Structure):
    _fields_ = [('addr', c.c_void_p), ('first', c.c_uint), ('step', c.c_uint)]


class Playback:
    def __init__(self, library, name, access=3):
        self.lib = c.CDLL(library)
        self.pcm = c.c_void_p()
        signatures = {
            'snd_pcm_open': [c.POINTER(c.c_void_p), c.c_char_p, c.c_int, c.c_int],
            'snd_pcm_set_params': [c.c_void_p, c.c_int, c.c_int, c.c_uint, c.c_uint, c.c_int, c.c_uint],
            'snd_pcm_get_params': [c.c_void_p, c.POINTER(c.c_ulong), c.POINTER(c.c_ulong)],
            'snd_pcm_writei': [c.c_void_p, c.c_void_p, c.c_ulong],
            'snd_pcm_delay': [c.c_void_p, c.POINTER(c.c_long)],
            'snd_pcm_avail_update': [c.c_void_p],
            'snd_pcm_forward': [c.c_void_p, c.c_ulong],
            'snd_pcm_rewind': [c.c_void_p, c.c_ulong],
            'snd_pcm_mmap_begin': [c.c_void_p, c.POINTER(c.POINTER(ChannelArea)), c.POINTER(c.c_ulong), c.POINTER(c.c_ulong)],
            'snd_pcm_mmap_commit': [c.c_void_p, c.c_ulong, c.c_ulong],
            'snd_pcm_sw_params_malloc': [c.POINTER(c.c_void_p)],
            'snd_pcm_sw_params_current': [c.c_void_p, c.c_void_p],
            'snd_pcm_sw_params_set_start_threshold': [c.c_void_p, c.c_void_p, c.c_ulong],
            'snd_pcm_sw_params': [c.c_void_p, c.c_void_p],
            'snd_pcm_sw_params_free': [c.c_void_p],
        }
        for function, arguments in signatures.items():
            getattr(self.lib, function).argtypes = arguments
        self.lib.snd_pcm_writei.restype = c.c_long
        for function in ('avail_update', 'forward', 'rewind', 'mmap_commit'):
            getattr(self.lib, 'snd_pcm_' + function).restype = c.c_long
        self.lib.snd_pcm_sw_params_free.restype = None
        for function in ('start', 'prepare', 'drain', 'drop', 'close', 'state', 'reset'):
            getattr(self.lib, 'snd_pcm_' + function).argtypes = [c.c_void_p]
        before = self.descriptors()
        self.check(self.lib.snd_pcm_open(c.byref(self.pcm), name.encode(), 0, 0), 'open')
        try:
            self.check(self.lib.snd_pcm_set_params(self.pcm, 2, access, 2, 44100, 1, 200000), 'set_params')
            size, period = c.c_ulong(), c.c_ulong()
            self.check(self.lib.snd_pcm_get_params(self.pcm, c.byref(size), c.byref(period)), 'get_params')
            self.capacity, self.period = size.value, period.value
            assert 0 < self.period < self.capacity <= 16384, (self.capacity, self.period)
            descriptors = self.descriptors() - before
            assert len(descriptors) == 1, descriptors
            self.fd = descriptors.pop()
            self.software_start()
        except BaseException:
            self.close()
            raise

    @staticmethod
    def check(result, operation):
        if result < 0:
            raise RuntimeError((operation, result))
        return result

    @staticmethod
    def descriptors():
        result = set()
        for fd in range(3, 256):
            try:
                info = os.fstat(fd)
            except OSError:
                continue
            if stat.S_ISCHR(info.st_mode) and info.st_rdev == 0x0E03:
                result.add(fd)
        return result

    def software_start(self):
        parameters = c.c_void_p()
        self.check(self.lib.snd_pcm_sw_params_malloc(c.byref(parameters)), 'sw_malloc')
        try:
            self.check(self.lib.snd_pcm_sw_params_current(self.pcm, parameters), 'sw_current')
            self.check(self.lib.snd_pcm_sw_params_set_start_threshold(
                self.pcm, parameters, self.capacity + 1), 'sw_threshold')
            self.check(self.lib.snd_pcm_sw_params(self.pcm, parameters), 'sw_apply')
        finally:
            self.lib.snd_pcm_sw_params_free(parameters)

    def call(self, operation):
        return self.check(getattr(self.lib, 'snd_pcm_' + operation)(self.pcm), operation)

    def write(self, mono):
        samples = array.array('h', [sample for sample in mono for _ in range(2)])
        buffer = c.create_string_buffer(samples.tobytes())
        written = self.check(self.lib.snd_pcm_writei(self.pcm, buffer, len(mono)), 'writei')
        assert written == len(mono), (written, len(mono))

    def oss_delay(self):
        data = array.array('i', [0])
        fcntl.ioctl(self.fd, 0x80045017, data, True)
        assert data[0] % 4 == 0, data
        return data[0] // 4

    def compare_delay(self):
        before = self.oss_delay()
        delay = c.c_long()
        self.check(self.lib.snd_pcm_delay(self.pcm, c.byref(delay)), 'delay')
        after = self.oss_delay()
        assert after <= delay.value <= before, (before, delay.value, after)
        return {'oss_before_frames': before, 'alsa_frames': delay.value, 'oss_after_frames': after}

    def close(self):
        if self.pcm:
            self.lib.snd_pcm_close(self.pcm)
            self.pcm = c.c_void_p()


def clock(library, name):
    a, b = Playback(library, name), None
    try:
        b = Playback(library, name)
        a.write([1200] * a.capacity)
        assert a.lib.snd_pcm_state(a.pcm) == 2  # PREPARED: no physical progress yet.
        time.sleep(.05)
        initial = a.compare_delay()
        assert initial['alsa_frames'] == a.capacity
        b.write([1800] * b.period)
        b.call('drain')  # A paused independent stream must not be charged for B.
        assert a.compare_delay()['alsa_frames'] == a.capacity
        a.call('start')
        samples = []
        for _ in range(4):
            samples.append(a.compare_delay())
            time.sleep(.01)
        assert any(row['alsa_frames'] > 0 for row in samples), samples
        for _ in range(64):
            a.write([1200] * a.period)
            samples.append(a.compare_delay())
        a.call('drain')
        assert a.oss_delay() == 0
        print('ALSA_OSS_CLOCK_PASS', json.dumps({'pcm': name, 'capacity_frames': a.capacity,
              'period_frames': a.period, 'initial': initial, 'samples': samples}), flush=True)
    finally:
        if b is not None:
            b.close()
        a.close()


def exact(library, name):
    pcm = Playback(library, name)
    try:
        print('ALSA_OSS_EXACT_READY', flush=True)
        time.sleep(3)
        for _ in range(6):
            for index, frames in enumerate((1, 16, 128, 441, 1024)):
                pcm.call('prepare')
                magnitude = 4000 + index * 2000
                pcm.write([magnitude] * (frames - 1) + [-magnitude])
                pcm.call('drain')
                assert pcm.oss_delay() == 0
                time.sleep(.075)
        print('ALSA_OSS_EXACT_PASS', json.dumps({'pcm': name, 'source_frames': 9660}), flush=True)
    finally:
        pcm.close()


def boundaries(library, name):
    pcm = Playback(library, name)
    try:
        pcm.write([1200] * pcm.capacity)
        pcm.call('start')
        # Deliberately do not observe the pointer for longer than a full ring.
        for round_index in range(3):
            time.sleep(.7)
            assert pcm.compare_delay()['alsa_frames'] == 0
            available = pcm.lib.snd_pcm_avail_update(pcm.pcm)
            assert available == pcm.capacity, (round_index, available, pcm.capacity)
            if round_index != 2:
                pcm.write([1200] * pcm.capacity)
        print('ALSA_OSS_BOUNDARY full_ring_gaps PASS', flush=True)
        if name == 'brook_oss':
            pcm.call('reset')
            assert pcm.compare_delay()['alsa_frames'] == 0
            assert pcm.lib.snd_pcm_avail_update(pcm.pcm) == pcm.capacity
            pcm.write([1200] * pcm.period)
            pcm.call('drain')
            pcm.call('prepare')
            pcm.write([1200] * pcm.period)
            pcm.call('reset')
            assert pcm.lib.snd_pcm_avail_update(pcm.pcm) == -errno.EPIPE
            pcm.call('prepare')
            assert pcm.lib.snd_pcm_forward(pcm.pcm, 128) == 128
            assert pcm.lib.snd_pcm_avail_update(pcm.pcm) == -errno.EPIPE
            pcm.call('prepare')
            pcm.write([1200] * pcm.period)
            assert pcm.lib.snd_pcm_rewind(pcm.pcm, 1) == 1
            assert pcm.lib.snd_pcm_avail_update(pcm.pcm) == -errno.EPIPE
            pcm.call('prepare')
            pcm.write([1200])
            pcm.call('drain')
            print('ALSA_OSS_BOUNDARY reset_origin_forward_rewind_guard PASS', flush=True)
    finally:
        pcm.close()
    if name == 'brook_oss':
        pcm = Playback(library, name, access=0)  # MMAP_INTERLEAVED with transfer-on-commit.
        try:
            areas = c.POINTER(ChannelArea)()
            offset, frames = c.c_ulong(), c.c_ulong(128)
            pcm.check(pcm.lib.snd_pcm_mmap_begin(pcm.pcm, c.byref(areas), c.byref(offset), c.byref(frames)), 'mmap_begin')
            assert frames.value >= 64 and areas[0].step == 32 and areas[0].first == 0
            samples = array.array('h', [1200, -1200] * frames.value)
            c.memmove(areas[0].addr + offset.value * 4, samples.tobytes(), len(samples) * 2)
            assert pcm.lib.snd_pcm_mmap_commit(pcm.pcm, offset.value, 64) == 64
            assert pcm.compare_delay()['alsa_frames'] == 64, 'uncommitted mmap bytes were charged'
            pcm.call('drain')
            assert pcm.oss_delay() == 0
            print('ALSA_OSS_BOUNDARY partial_mmap_commit PASS', flush=True)
        finally:
            pcm.close()
    print('ALSA_OSS_BOUNDARIES_PASS', json.dumps({'pcm': name}), flush=True)


if __name__ == '__main__':
    library, name, mode = sys.argv[1:4]
    if mode == 'clock':
        clock(library, name)
    elif mode == 'exact':
        exact(library, name)
    elif mode == 'boundaries':
        boundaries(library, name)
    else:
        raise ValueError('Choose clock, exact or boundaries')

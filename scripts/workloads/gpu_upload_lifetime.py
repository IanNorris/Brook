#!/usr/bin/env python3
"""Bounded native Brook legacy-GPU upload/teardown reuse diagnostic.

Uses Brook-specific syscalls; no game assets or rendered frames are required.
A zero destroy return is not proof of host retirement: also inspect the saved
serial log for verified teardown, heap checks and repeated context-slot reuse.
"""
import argparse
import ctypes
import json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cycles', type=int, default=40)
    args = parser.parse_args()
    if not 2 <= args.cycles <= 100:
        parser.error('Use 2..100 cycles')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    def call(number, *args):
        value = libc.syscall(ctypes.c_long(number), *args)
        if value < 0:
            raise OSError(ctypes.get_errno(), f'Brook GPU syscall {number:#x} failed')
        return value
    data = ctypes.create_string_buffer(bytes(range(64)))
    records = []
    resources = set()
    contexts = []
    for cycle in range(args.cycles):
        context = call(0xB10)
        assert context >= 256
        uploaded = []
        for _ in range(2):
            resource = call(0xB16, ctypes.c_long(context), ctypes.cast(data, ctypes.c_void_p),
                            ctypes.c_ulong(64))
            assert resource > 0 and resource not in resources
            resources.add(resource)
            uploaded.append(resource)
        assert call(0xB15, ctypes.c_long(context)) == 0
        contexts.append(context)
        records.append(dict(cycle=cycle, context=context, resources=uploaded))
    assert len(set(contexts)) == 1, 'Expected verified teardown to permit the same slot each cycle'
    print(json.dumps(dict(cycles=args.cycles, uploads=len(resources), records=records,
                          scope='Actual Brook legacy upload ABI, two uploads per context, '
                          'unique resource IDs and repeated context-slot reuse. Check serial '
                          'verified teardown and heap/runtime separately; not timeout injection '
                          'or rendering correctness.')))


if __name__ == '__main__':
    main()

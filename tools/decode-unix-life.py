#!/usr/bin/env python3
"""Decode a paused-QEMU raw memory dump of g_unixLifeTrace (265248 bytes)."""
import argparse
import json
import struct

EVENTS = ('invalid Allocate Publish Retain Release RegistryPin Finalize DirectFree '
          'HeapAttempt HeapFree Bind Listen SnapshotInstall CopyRetain Claim '
          'HeapAllocate HeapWriteFree').split()
HEADER = struct.Struct('<QQQII')
WATCH = struct.Struct('<QQII')
RECORD = struct.Struct('<QQQQQIIHHiII')
SIZE = HEADER.size + 128 * WATCH.size + 4096 * RECORD.size

def decode(data, handle=None):
    if len(data) != SIZE:
        raise ValueError(f'expected {SIZE} raw bytes, got {len(data)}')
    sequence, overwritten, saturated, enabled, generation = HEADER.unpack_from(data)
    result = {'sequence': sequence, 'overwritten': overwritten,
              'watch_saturated': saturated, 'enabled': enabled,
              'last_generation': generation, 'records': []}
    offset = HEADER.size + 128 * WATCH.size
    for slot in range(4096):
        values = RECORD.unpack_from(data, offset + slot * RECORD.size)
        seq, address, caller, other, size, prev, nxt, pid, tgid, fd, event, gen = values
        if not seq or seq > sequence or seq <= max(0, sequence - 4096):
            continue
        if (seq - 1) % 4096 != slot:
            raise ValueError(f'uncommitted/inconsistent slot {slot}: sequence {seq}')
        if handle is not None and address != handle:
            continue
        result['records'].append(dict(sequence=seq, handle=hex(address),
            caller=hex(caller), other=hex(other), size=size, previous=prev,
            next=nxt, pid=pid, tgid=tgid, fd=fd, generation=gen,
            event=EVENTS[event] if event < len(EVENTS) else str(event)))
    result['records'].sort(key=lambda record: record['sequence'])
    return result

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump')
    parser.add_argument('--handle', type=lambda value: int(value, 0))
    args = parser.parse_args()
    with open(args.dump, 'rb') as source:
        print(json.dumps(decode(source.read(), args.handle), indent=2))

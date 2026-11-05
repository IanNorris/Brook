#!/usr/bin/env python3
"""Exercise the safety-critical firmware inventory parser with malformed inputs."""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from efi_boot_inventory import parse_inventory, parse_option


def report(items):
    return ('BRKBOOTV1\n' + ''.join(k + '=' + v.hex() + '\n' for k, v in items) + 'END\n').encode()


def rejected(fn, value):
    try:
        fn(value)
    except (ValueError, UnicodeError):
        return
    raise AssertionError('malformed inventory accepted')


def main():
    end = b'\x7f\xff\x04\x00'
    def file_node(value):
        return b'\x04\x04' + struct.pack('<H', 4 + len(value)) + value
    def option(path):
        return struct.pack('<IH', 1, len(path)) + b'X\0\0\0' + path

    valid = option(file_node('\\EFI\\BOOT\\BOOTX64.EFI\0'.encode('utf-16-le')) + end)
    parsed = parse_inventory(report([('BootOrder', b'\0\0'), ('BootCurrent', b'\1\0'),
                                     ('BootNext', b'\2\0'), ('Boot0000', valid)]))
    assert parsed['missing_or_unparsed_referenced_entries'] == ['Boot0001', 'Boot0002']
    rejected(parse_option, option(file_node('x\0y\0'.encode('utf-16-le')) + end))
    rejected(parse_option, option(b'\x04\x04\x02\x00' + end))
    rejected(parse_option, option(end[:-1]))
    rejected(parse_inventory, report([('BootOrder', b'\0\0\0\0')]))
    print('Firmware inventory parser: PASS')


if __name__ == '__main__':
    main()

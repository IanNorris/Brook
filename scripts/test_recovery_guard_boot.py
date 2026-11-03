#!/usr/bin/env python3
"""Exercise the experimental EFI guard using private copies of a SATA fixture."""
import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
import zlib

from test_recovery_menu_boot import boot


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--firmware', type=Path, required=True)
    p.add_argument('--efi', type=Path, required=True)
    p.add_argument('--identity', required=True)
    p.add_argument('--normal-marker', required=True)
    p.add_argument('--recovery-marker', required=True)
    a = p.parse_args()
    if not re.fullmatch('[0-9a-f]{96}', a.identity) or a.normal_marker == a.recovery_marker:
        p.error('expected a 96-digit identity and distinct payload markers')
    source = a.image.resolve(strict=True)
    if not source.is_file() or source.stat().st_size != 128 * 1024 * 1024:
        p.error('expected a disposable 128 MiB SATA fixture')
    firmware, efi = a.firmware.resolve(strict=True), a.efi.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='brook-guard-boot-') as tmp:
        root = Path(tmp)
        for scenario in ('clean', 'pending', 'mismatch', 'missing', 'truncated',
                         'corrupt', 'wrong-identity', 'pending-no-config'):
            work = root / scenario; work.mkdir()
            disk = work / 'disk.raw'; shutil.copyfile(source, disk)
            media = str(disk) + '@@1048576'
            subprocess.run(['mcopy', '-o', '-i', media, str(efi), '::/EFI/BOOT/BOOTX64.EFI'], check=True)
            def guard(state, name):
                record = bytearray(128); record[:8] = b'BRKRCV01'
                struct.pack_into('<QI', record, 8, 1, state)
                record[24:72] = bytes.fromhex(a.identity)
                if scenario == 'wrong-identity':
                    record[24] ^= 1
                struct.pack_into('<I', record, 124, zlib.crc32(record[:124]))
                if scenario == 'corrupt':
                    record[124] ^= 1
                if scenario == 'truncated':
                    del record[-1:]
                path = work / name; path.write_bytes(record)
                subprocess.run(['mcopy', '-o', '-i', media, str(path), '::/RECOVERY/' + name], check=True)
            guard(2 if scenario in ('pending', 'pending-no-config') else 1, 'GUARD0.BIN')
            if scenario != 'missing':
                guard(2 if scenario in ('pending', 'mismatch', 'pending-no-config') else 1, 'GUARD1.BIN')
            else:
                subprocess.run(['mdel', '-i', media, '::/RECOVERY/GUARD1.BIN'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if subprocess.run(['mtype', '-i', media, '::/RECOVERY/GUARD1.BIN'],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                    raise RuntimeError('missing-record fixture still contains guard 1')
            if scenario == 'pending-no-config':
                subprocess.run(['mdel', '-i', media, '::/BROOK.CFG'], check=True)
            with disk.open('rb') as stream:
                before = hashlib.file_digest(stream, 'sha256').digest()
            normal = scenario == 'clean'
            boot(disk, firmware, work, False,
                 a.normal_marker if normal else a.recovery_marker, 90, True,
                 a.recovery_marker if normal else a.normal_marker, sata_snapshot=normal)
            with disk.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').digest() != before:
                    raise RuntimeError('guard test changed its source disk')
            print('GUARD_' + scenario.upper().replace('-', '_') + '_PASS', flush=True)


if __name__ == '__main__':
    main()

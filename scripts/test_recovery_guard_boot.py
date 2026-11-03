#!/usr/bin/env python3
"""Exercise the experimental EFI guard using private copies of a SATA fixture."""
import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
import zlib

from test_recovery_menu_boot import boot
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_update_protocol import record


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
                         'corrupt', 'wrong-identity', 'pending-no-config',
                         'fallback-clean', 'fallback-pending', 'legacy-clean', 'legacy-pending'):
            work = root / scenario; work.mkdir()
            disk = work / 'disk.raw'; shutil.copyfile(source, disk)
            media = str(disk) + '@@1048576'
            subprocess.run(['mcopy', '-o', '-i', media, str(efi), '::/EFI/BOOT/BOOTX64.EFI'], check=True)
            if scenario.startswith(('fallback-', 'legacy-')):
                kernel = subprocess.check_output(['mtype', '-i', media, '::/KERNEL/BROOK.ELF'])
                initrd = subprocess.check_output(['mtype', '-i', media, '::/INITRD.IMG'])
                config = work / 'BROOK.CFG'
                config.write_bytes(subprocess.check_output(['mtype', '-i', media, '::/BROOK.CFG']) + b'\nUPDATE_SLOTS=1\n')
                subprocess.run(['mcopy', '-o', '-i', media, str(config), '::/BROOK.CFG'], check=True)
                for directory in ('UPDATES', 'UPDATES/A', 'UPDATES/B'):
                    subprocess.run(['mmd', '-i', media, '::/' + directory], check=True)
                for slot, generation, name in ((0, 2, 'A'), (1, 1, 'B')):
                    # A cannot boot even if its hash were ignored; B is valid
                    # only in the fallback case. Legacy leaves neither slot
                    # executable, even if selector checks were bypassed.
                    kernel_file = work / ('kernel-' + name)
                    data = bytearray(kernel)
                    if name == 'A' or scenario.startswith('legacy-'): data[0] = 0
                    kernel_file.write_bytes(data)
                    initrd_file = work / ('initrd-' + name); initrd_file.write_bytes(initrd)
                    selector = bytearray(record(generation, slot, kernel, initrd))
                    if scenario.startswith('legacy-'): selector[124] ^= 1
                    select_file = work / ('SELECT' + str(slot) + '.BIN'); select_file.write_bytes(selector)
                    for path, target in ((kernel_file, '::/UPDATES/' + name + '/KERNEL.ELF'),
                                         (initrd_file, '::/UPDATES/' + name + '/INITRD.IMG'),
                                         (select_file, '::/UPDATES/' + select_file.name)):
                        subprocess.run(['mcopy', '-o', '-i', media, str(path), target], check=True)
                if scenario.startswith('fallback-'):
                    # A and root cannot boot, so a normal marker proves B was selected.
                    subprocess.run(['mdel', '-i', media, '::/KERNEL/BROOK.ELF'], check=True)
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
            guard(2 if 'pending' in scenario else 1, 'GUARD0.BIN')
            if scenario != 'missing':
                guard(2 if 'pending' in scenario or scenario == 'mismatch' else 1, 'GUARD1.BIN')
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
            normal = scenario == 'clean' or scenario.endswith('-clean')
            boot(disk, firmware, work, False,
                 a.normal_marker if normal else a.recovery_marker, 90, True,
                 a.recovery_marker if normal else a.normal_marker, sata_snapshot=normal)
            with disk.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').digest() != before:
                    raise RuntimeError('guard test changed its source disk')
            print('GUARD_' + scenario.upper().replace('-', '_') + '_PASS', flush=True)
            # Keep peak temporary storage to one disk copy, not one per case.
            shutil.rmtree(work)


if __name__ == '__main__':
    main()

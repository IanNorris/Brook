#!/usr/bin/env python3
"""Stage an SSD maintenance pair into an inactive, already verified USB slot.

The current slot and root recovery are never modified. The overwritten slot
and both original selection records are saved and independently checked first.
Any failed/ambiguous operation requires inspection; this script never retries.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_update_protocol import record, KERNEL_MAX, INITRD_MAX


def readfile(path, size=None):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or (size is not None and info.st_size != size):
            raise ValueError('unexpected file type or size: ' + str(path))
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            return stream.read()
    finally:
        os.close(fd)


def parse(data):
    if len(data) != 128:
        raise ValueError('selection record size')
    magic, generation, slot, flags, kernel_size, initrd_size = struct.unpack('<8sQIIQQ', data[:40])
    if (magic != b'BROOKUP1' or not generation or slot not in (0, 1) or flags or
        not 64 <= kernel_size <= KERNEL_MAX or not 512 <= initrd_size <= INITRD_MAX or
        any(data[104:124]) or zlib.crc32(data[:124]) != int.from_bytes(data[124:], 'little')):
        raise ValueError('invalid selection record')
    return generation, slot, kernel_size, initrd_size


def executable(validator, data):
    return subprocess.run([str(validator)], input=data, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def verify_pair(updates, raw, validator):
    generation, slot, ks, rs = parse(raw)
    directory = updates / ('A' if slot == 0 else 'B')
    kernel = readfile(directory / 'KERNEL.ELF', KERNEL_MAX)
    initrd = readfile(directory / 'INITRD.IMG', INITRD_MAX)
    if hashlib.sha256(kernel[:ks]).digest() != raw[40:72] or hashlib.sha256(initrd[:rs]).digest() != raw[72:104]:
        raise ValueError('selection record does not match its slot')
    if not executable(validator, kernel[:ks]):
        raise ValueError('selection record contains an ineligible executable')
    return generation, slot, kernel, initrd


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def overwrite(path, data, exact_size):
    fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_size != exact_size or len(data) > exact_size:
            raise ValueError('slot file changed')
        offset = 0
        while offset < len(data):
            n = os.pwrite(fd, data[offset:offset + 1024 * 1024], offset)
            if n <= 0:
                raise OSError('short slot write')
            offset += n
        os.fsync(fd)
        if hasattr(os, 'posix_fadvise'):
            os.posix_fadvise(fd, 0, len(data), os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)
    actual = readfile(path, exact_size)
    if actual[:len(data)] != data:
        raise OSError('slot readback mismatch')


def stage(mount, payload, backup, expected_generation, validator):
    updates = mount / 'UPDATES'
    records = [readfile(updates / f'SELECT{i}.BIN', 128) for i in range(2)]
    pairs = [verify_pair(updates, r, validator) for r in records]
    if pairs[0][:2] != (expected_generation, 0) or pairs[1][:2] != (expected_generation - 1, 1):
        raise ValueError('unexpected USB slot generations; inspect before staging')
    kernel = readfile(payload / 'kernel.elf'); initrd = readfile(payload / 'INITRD.IMG')
    manifest = json.loads(readfile(payload / 'manifest.json'))
    for name, data in (('kernel.elf', kernel), ('INITRD.IMG', initrd)):
        expected = manifest['files'][name]
        if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
            raise ValueError('recovery payload hash mismatch')
    if not executable(validator, kernel):
        raise ValueError('recovery kernel is ineligible for EFI boot')
    # Backup must survive removal/failure of the USB volume.
    if os.stat(backup.parent).st_dev == os.stat(mount).st_dev:
        raise ValueError('backup destination must be on a different filesystem')
    new_record = record(expected_generation + 1, 1, kernel, initrd)
    backup.mkdir(parents=True, exist_ok=False, mode=0o700)
    saved = {'SELECT0.BIN': records[0], 'SELECT1.BIN': records[1],
             'KERNEL-B.ELF': pairs[1][2], 'INITRD-B.IMG': pairs[1][3]}
    for name, data in saved.items():
        with (backup / name).open('xb') as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        if readfile(backup / name) != data:
            raise OSError('backup readback mismatch')
    sync_dir(backup); sync_dir(backup.parent)
    receipt = {'state': 'backup_verified', 'current_generation': expected_generation,
               'new_generation': expected_generation + 1, 'payload_id': manifest['payload_id']}
    def save():
        tmp = backup / 'receipt.tmp'
        with tmp.open('w') as f:
            json.dump(receipt, f, indent=2); f.write('\n'); f.flush(); os.fsync(f.fileno())
        tmp.replace(backup / 'receipt.json'); sync_dir(backup)
    save()
    try:
        receipt['state'] = 'slot_write_started'; save()
        overwrite(updates / 'B/KERNEL.ELF', kernel, KERNEL_MAX)
        overwrite(updates / 'B/INITRD.IMG', initrd, INITRD_MAX)
        receipt['state'] = 'slot_verified'; save()
        overwrite(updates / 'SELECT1.BIN', new_record, 128)
        sync_dir(updates)
        if readfile(updates / 'SELECT0.BIN', 128) != records[0]:
            raise OSError('current selection record unexpectedly changed')
        verify_pair(updates, new_record, validator)
        receipt['state'] = 'selected_and_verified'; save()
    except Exception as e:
        receipt.update(state='needs_inspection', error=str(e)); save()
        raise
    return receipt


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mount', required=True, type=Path)
    p.add_argument('--payload', required=True, type=Path)
    p.add_argument('--backup', required=True, type=Path)
    p.add_argument('--expected-uuid', required=True)
    p.add_argument('--expected-generation', required=True, type=int)
    p.add_argument('--elf-validator', required=True, type=Path,
                   help='Host executable built from tools/update_elf_check.cpp')
    a = p.parse_args()
    mounted = subprocess.check_output(['findmnt', '-rn', '-T', str(a.mount), '-o', 'SOURCE,FSTYPE,TARGET'], text=True).strip().split()
    if len(mounted) != 3 or mounted[1] != 'vfat' or Path(mounted[2]) != a.mount.resolve():
        raise ValueError('expected the mounted USB FAT volume')
    actual_uuid = subprocess.check_output(['lsblk', '-no', 'UUID', mounted[0]], text=True).strip()
    if actual_uuid != a.expected_uuid:
        raise ValueError('USB volume UUID mismatch')
    print(json.dumps(stage(a.mount, a.payload, a.backup, a.expected_generation, a.elf_validator.resolve(strict=True)), indent=2))


if __name__ == '__main__':
    main()

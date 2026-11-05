#!/usr/bin/env python3
"""Prepare a matched hardware kernel/initrd pair without touching a device.

Every driver already present in the source initrd must have a matching module
in the supplied build, with the requested module ABI. The source policy and
other initrd contents are preserved. This script does not activate the pair.
The supplied ABI is not extracted from the kernel; a separate exact-pair boot
test is required to verify kernel/module compatibility.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from hardware_recovery_fsck import validate_checker

MAGIC = 0x4B524F42
KERNEL_MAX = 8 * 1024 * 1024
INITRD_MAX = 64 * 1024 * 1024


def regular(path, maximum):
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
            raise ValueError(f'{path}: not a bounded regular file')
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            return stream.read(maximum + 1)
    finally:
        os.close(fd)


def module_version(data):
    section = ELFFile(io.BytesIO(data)).get_section_by_name('.modinfo')
    if section is None or len(section.data()) < 8:
        raise ValueError('missing module metadata')
    magic, version = struct.unpack_from('<II', section.data())
    if magic != MAGIC:
        raise ValueError('invalid module magic')
    return version


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare(source, build, abi, payload_id, output, expected_kernel_sha256, panic_exporter=None):
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,95}', payload_id):
        raise ValueError('invalid payload ID')
    kernel = regular(build / 'BROOK.elf', KERNEL_MAX)
    if not re.fullmatch(r'[0-9a-f]{64}', expected_kernel_sha256) or digest(kernel) != expected_kernel_sha256:
        raise ValueError('kernel hash does not match the explicitly selected build')
    source_data = regular(source, INITRD_MAX)
    # Enumerate the exact bytes that will be copied, not a second read of the
    # source pathname that could refer to different contents by this point.
    with tempfile.TemporaryDirectory(prefix='brook-hardware-pair-') as directory:
        captured = Path(directory) / 'source.img'
        captured.write_bytes(source_data)
        listing = subprocess.check_output(['mdir', '-a', '-b', '-i', str(captured), '::/DRIVERS/*'], text=True)
    names = [line.rsplit('/', 1)[-1] for line in listing.splitlines() if line]
    if not names or len({n.lower() for n in names}) != len(names):
        raise ValueError('missing or ambiguous source modules')
    inventory = []
    for name in names:
        if not re.fullmatch(r'[A-Za-z0-9_-]+\.[Mm][Oo][Dd]', name):
            raise ValueError('unexpected module name')
        module = build / 'drivers' / name.lower()
        contents = regular(module, 16 * 1024 * 1024)
        if module_version(contents) != abi:
            raise ValueError(f'{name}: mismatched module ABI')
        inventory.append((name, module, digest(contents)))
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    try:
        image = output / 'INITRD.IMG'
        image.write_bytes(source_data)
        (output / 'kernel.elf').write_bytes(kernel)
        marker = output / 'hardware-payload-id'
        marker.write_text(payload_id + '\n')
        for name, module, _ in inventory:
            subprocess.run(['mcopy', '-o', '-i', str(image), str(module), '::/DRIVERS/' + name], check=True)
        subprocess.run(['mcopy', '-o', '-i', str(image), str(marker), '::/ETC/hardware-payload-id'], check=True)
        export_info = None
        if panic_exporter is not None:
            export_info = validate_checker(panic_exporter)
            subprocess.run(['mcopy', '-o', '-i', str(image), str(panic_exporter), '::/BOOT/PANICLOG.ELF'], check=True)
            copied = subprocess.check_output(['mtype', '-i', str(image), '::/BOOT/PANICLOG.ELF'])
            if len(copied) != export_info['bytes'] or digest(copied) != export_info['sha256']:
                raise RuntimeError('panic exporter readback mismatch')
        for name, _, expected in inventory:
            data = subprocess.check_output(['mtype', '-i', str(image), '::/DRIVERS/' + name])
            if digest(data) != expected:
                raise RuntimeError(f'{name}: readback mismatch')
        if subprocess.check_output(['mtype', '-i', str(image), '::/ETC/hardware-payload-id']) != marker.read_bytes():
            raise RuntimeError('payload ID readback mismatch')
        result = {'scope': 'prepared private files only; no deployment or boot test',
                  'payload_id': payload_id, 'module_abi': abi,
                  'source_initrd_sha256': digest(source_data),
                  'kernel_sha256': digest(kernel), 'initrd_sha256': digest(image.read_bytes()),
                  'modules': {name: sha for name, _, sha in inventory}}
        if export_info is not None: result['panic_journal_exporter'] = export_info
        (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        return result
    except BaseException:
        shutil.rmtree(output)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-initrd', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--abi', type=int, required=True)
    p.add_argument('--payload-id', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--expected-kernel-sha256', required=True)
    p.add_argument('--panic-exporter', type=Path, help='Embed the static panic journal export utility')
    a = p.parse_args()
    print(json.dumps(prepare(a.source_initrd, a.build, a.abi, a.payload_id, a.output,
                             a.expected_kernel_sha256, a.panic_exporter), indent=2))


if __name__ == '__main__':
    main()

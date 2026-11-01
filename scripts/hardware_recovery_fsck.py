"""Validate and embed a standalone fsck executable in a private initrd image."""
import hashlib
import os
import stat
import struct
import subprocess
from pathlib import Path


def validate_checker(path):
    path = Path(path)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(fd, 'rb') as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ValueError('checker must be a regular file')
        if not 64 <= info.st_size <= 8 * 1024 * 1024:
            raise ValueError('checker size outside supported range')
        data = source.read(8 * 1024 * 1024 + 1)
    if len(data) != info.st_size:
        raise ValueError('checker changed during read')
    if data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('checker must be little-endian ELF64')
    kind, machine = struct.unpack_from('<HH', data, 16)
    version = struct.unpack_from('<I', data, 20)[0]
    entry = struct.unpack_from('<Q', data, 24)[0]
    phoff = struct.unpack_from('<Q', data, 32)[0]
    ehsize, phentsize, phnum = struct.unpack_from('<HHH', data, 52)
    if kind != 2 or machine != 62 or version != 1 or ehsize != 64 or phentsize < 56 or not 1 <= phnum <= 128:
        raise ValueError('checker must be an x86-64 executable with program headers')
    if phoff > len(data) or phnum * phentsize > len(data) - phoff:
        raise ValueError('checker program headers exceed file bounds')
    entry_executable = False
    for i in range(phnum):
        off = phoff + i * phentsize
        typ = struct.unpack_from('<I', data, off)[0]
        if typ in (2, 3):
            raise ValueError('checker must not require a dynamic linker')
        if typ == 1:
            flags = struct.unpack_from('<I', data, off + 4)[0]
            start = struct.unpack_from('<Q', data, off + 8)[0]
            address = struct.unpack_from('<Q', data, off + 16)[0]
            size = struct.unpack_from('<Q', data, off + 32)[0]
            memory = struct.unpack_from('<Q', data, off + 40)[0]
            if start > len(data) or size > len(data) - start:
                raise ValueError('checker load segment exceeds file bounds')
            if size > memory or address >= 0x800000000000 or memory > 0x800000000000 - address:
                raise ValueError('checker load segment has invalid memory extent')
            if flags & 1 and size and address <= entry < address + size:
                entry_executable = True
    if not entry_executable:
        raise ValueError('checker entry must be in a file-backed executable segment')
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def embed_checker(initrd, checker):
    expected = validate_checker(checker)
    subprocess.run(['mcopy', '-o', '-i', str(initrd), str(checker), '::BOOT/E2FSCK.ELF'], check=True)
    copied = subprocess.check_output(['mtype', '-i', str(initrd), '::BOOT/E2FSCK.ELF'])
    if len(copied) != expected['bytes'] or hashlib.sha256(copied).hexdigest() != expected['sha256']:
        raise RuntimeError('checker initrd readback mismatch')
    return expected

#!/usr/bin/env python3
"""Stage a supervised Clearwater shell into a fresh copy of a boot initrd."""
import argparse
import hashlib
import io
from pathlib import Path
import shutil
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile

CLEARWATER_START = b'run --service --once --console --uid 0 /boot/CWSUP.ELF /boot/CWSHELL.ELF'
CLEARWATER_EXECUTABLES = ('CWSUP.ELF', 'CWSHELL.ELF')
STARTUP = '::/BOOT/INIT.RC'


def add_clearwater_start(script):
    if not script or len(script) > 65536 or b'\0' in script:
        raise ValueError('invalid startup script')
    if any(b'/boot/cwsup.elf' in line.lower() or b'/boot/cwshell.elf' in line.lower()
           for line in script.splitlines() if not line.lstrip().startswith(b'#')):
        raise ValueError('startup script already launches Clearwater')
    return script + (b'' if script.endswith(b'\n') else b'\n') + CLEARWATER_START + b'\n'


def embed_clearwater_executables(image, supervisor, shell):
    hashes = {}
    with tempfile.TemporaryDirectory(prefix='brook-clearwater-executables-') as directory:
        payloads = zip(CLEARWATER_EXECUTABLES, (supervisor, shell))
        for name, path in payloads:
            with path.open('rb') as stream:
                data = stream.read(8 * 1024 * 1024 + 1)
            if not data.startswith(b'\x7fELF') or len(data) > 8 * 1024 * 1024:
                raise ValueError(f'{name} must be a bounded ELF executable')
            elf = ELFFile(io.BytesIO(data))
            if (elf.header['e_machine'] != 'EM_X86_64'
                or elf.header['e_type'] not in ('ET_EXEC', 'ET_DYN')
                or not elf.header['e_entry']
                or any(segment.header.p_type == 'PT_INTERP' for segment in elf.iter_segments())):
                raise ValueError(f'{name} must be a static x86-64 executable')
            captured = Path(directory) / name
            captured.write_bytes(data)
            dest = '::/BOOT/' + name
            subprocess.run(['mcopy', '-o', '-i', image, captured, dest], check=True)
            if subprocess.check_output(['mtype', '-i', image, dest]) != data:
                raise RuntimeError(f'{name} readback mismatch')
            hashes[name] = hashlib.sha256(data).hexdigest()
    return hashes


def stage(source, output, supervisor, shell):
    if output.exists():
        raise FileExistsError(output)
    with tempfile.TemporaryDirectory(prefix='brook-clearwater-stage-', dir=output.parent) as directory:
        image = Path(directory) / 'INITRD.IMG'
        shutil.copyfile(source, image)
        script = add_clearwater_start(subprocess.check_output(['mtype', '-i', image, STARTUP]))
        embed_clearwater_executables(image, supervisor, shell)
        subprocess.run(['mcopy', '-o', '-i', image, '-', STARTUP], input=script, check=True)
        if subprocess.check_output(['mtype', '-i', image, STARTUP]) != script:
            raise RuntimeError('Clearwater startup readback mismatch')
        output.hardlink_to(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source-initrd', 'output', 'clearwater-supervisor', 'clearwater-shell'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    stage(args.source_initrd, args.output, args.clearwater_supervisor, args.clearwater_shell)


if __name__ == '__main__':
    main()

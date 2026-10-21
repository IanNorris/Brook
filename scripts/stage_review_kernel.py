#!/usr/bin/env python3
"""Stage a matched kernel/module build into a stopped private review copy.

Run in the development Nix shell. No build, VM launch, user-data disk writes or
in-place update of a running review. The caller supplies the exact kernel hash
and module ABI; every module is validated and round-tripped through private FAT.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--review', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT/'build/release/kernel')
    parser.add_argument('--expected-kernel-sha256', required=True)
    parser.add_argument('--module-abi', type=int, required=True)
    args = parser.parse_args()
    review, build = args.review.resolve(), args.build.resolve()
    review.relative_to(ROOT/'build/reviews')
    boot = review/'boot.img'
    if boot.is_symlink() or boot.stat().st_nlink != 1:
        raise RuntimeError('Use a private, non-hardlinked boot image')
    artifacts = review/'artifacts/kernel'
    if artifacts.resolve() != artifacts or not artifacts.is_dir():
        raise RuntimeError('Use an existing private artifact directory without symlinks')
    for entry in Path('/proc').glob('[0-9]*/exe'):
        try:
            executable = entry.resolve(strict=True).name
        except OSError:
            continue
        if 'qemu-system' in executable:
            raise RuntimeError('All QEMU instances must be stopped before kernel staging')
    kernel = build/'BROOK.elf'
    if sha(kernel) != args.expected_kernel_sha256:
        raise RuntimeError('Built kernel differs from the requested hash')
    symbols = build/'ksym_addr_table.cpp'
    if not symbols.is_file() or not symbols.stat().st_size:
        raise RuntimeError('Incomplete build: missing or empty ksym_addr_table.cpp; nothing staged')
    modules = sorted((build/'drivers').glob('*.mod'))
    if not modules:
        raise RuntimeError('No built modules')
    inventory = []
    for module in modules:
        with module.open('rb') as stream:
            section = ELFFile(stream).get_section_by_name('.modinfo')
            if section is None:
                raise RuntimeError('Missing module metadata: '+module.name)
            magic, abi = struct.unpack_from('<II', section.data())
        if magic != 0x4b524f42 or abi != args.module_abi:
            raise RuntimeError('Mismatched module ABI: '+module.name)
        inventory.append(dict(name=module.name, abi=abi, sha256=sha(module)))
    lock = (review/'.launch.lock').open('a+')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    temporary = review/('boot.stage-abi'+str(args.module_abi)+'.img')
    verified = review/'staged-driver-verification'
    if temporary.exists() or verified.exists():
        raise RuntimeError('Previous staging evidence exists; inspect it instead of replacing it')
    subprocess.run(['cp','--reflink=auto','--sparse=always',str(boot),str(temporary)],check=True)
    subprocess.run(['mdel','-i',str(temporary),'::/DRIVERS/*.MOD'],check=True)
    subprocess.run(['mcopy','-o','-i',str(temporary),*[str(p) for p in modules],'::/DRIVERS/'],check=True)
    verified.mkdir()
    for module, row in zip(modules, inventory):
        exported = verified/module.name
        subprocess.run(['mcopy','-i',str(temporary),'::/DRIVERS/'+module.name,str(exported)],check=True)
        if sha(exported) != row['sha256']:
            raise RuntimeError('FAT module round-trip failed: '+module.name)
    for module in (artifacts/'drivers').glob('*.mod'):
        module.unlink() # private copy only; stale ABI providers must not survive
    for module in modules:
        shutil.copy2(module,artifacts/'drivers'/module.name)
    shutil.copy2(kernel,artifacts/'BROOK.elf')
    shutil.copy2(symbols,artifacts/'ksym_addr_table.cpp')
    if sha(kernel) != args.expected_kernel_sha256 or sha(artifacts/'BROOK.elf') != args.expected_kernel_sha256:
        raise RuntimeError('Kernel changed during staging; inspect this review')
    for module, row in zip(modules, inventory):
        if sha(module) != row['sha256'] or sha(artifacts/'drivers'/module.name) != row['sha256']:
            raise RuntimeError('Module changed during staging: '+module.name)
    os.replace(temporary,boot)
    report=dict(passed=True,kernel_sha256=args.expected_kernel_sha256,symbol_table_sha256=sha(symbols),module_abi=args.module_abi,
                build=str(build),review=str(review),modules=inventory,scope='Stopped private boot/artifact kernel pair; data/nix/tmp disks untouched')
    (review/'staged-kernel.json').write_text(json.dumps(report,indent=2)+'\n')
    print('STAGED_KERNEL_MODULES_PASS',args.module_abi,len(modules),flush=True)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

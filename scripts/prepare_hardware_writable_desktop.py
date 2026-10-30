#!/usr/bin/env python3
"""Freeze a writable-store-preferred desktop with accepted RAM recovery fallback.

Produces a new kernel/initrd pair only. STORE.IMG/STORE.OK are not copied to any
USB. The provisioner must verify the complete file before publishing STORE.OK.
"""
import argparse
import hashlib
import json
import subprocess
import os
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--store',required=True,type=Path)
    p.add_argument('--recovery-probe',required=True,type=Path)
    p.add_argument('--maintenance-probe',required=True,type=Path)
    p.add_argument('--waylandd',type=Path)
    p.add_argument('--display-idle-seconds',type=int,default=300)
    p.add_argument('--build',type=Path,default=ROOT/'build/release')
    p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();store=a.store.resolve(strict=True);out=a.output.absolute()
    m=json.loads((store/'manifest.json').read_text());image=store/'STORE.IMG'
    if not image.is_file() or image.is_symlink() or image.stat().st_size!=m['bytes'] or sha(image)!=m['sha256']:
        p.error('Invalid source store image')
    command=['python3',str(ROOT/'scripts/prepare_hardware_ram_desktop.py'),
        '--recovery-probe',str(a.recovery_probe),'--maintenance-probe',str(a.maintenance_probe),
        '--build',str(a.build),'--output',str(out),'--display-idle-seconds',str(a.display_idle_seconds)]
    if a.waylandd:command+=['--waylandd',str(a.waylandd)]
    subprocess.run(command,check=True,cwd=ROOT)
    subprocess.run([os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',
        str(ROOT/'scripts/workloads/writable_desktop_check.c'),'-o',str(out/'STOREQA.ELF')],check=True,cwd=ROOT)
    subprocess.run(['mcopy','-o','-i',str(out/'INITRD.IMG'),str(out/'STOREQA.ELF'),'::BOOT/STOREQA.ELF'],check=True,cwd=ROOT)
    record=out/'writable-desktop';record.write_text(f"BRKSTORE1 {m['bytes']} {m['uuid']}\n")
    subprocess.run(['mcopy','-o','-i',str(out/'INITRD.IMG'),str(record),'::ETC/writable-desktop'],check=True,cwd=ROOT)
    manifest=json.loads((out/'manifest.json').read_text())
    manifest.update(scope='Writable desktop preferred; accepted immutable RAM fallback',
        ephemeral_home=False,mutable_usb_application_mount=True,
        home_persistence='persistent only after writable-store admission; RAM fallback remains volatile',
        writable_desktop={'source':'/media/usb/STORE.IMG','readiness':'/media/usb/STORE.OK',
            'bytes':m['bytes'],'uuid':m['uuid'],'initial_sha256':m['sha256'],
            'store_artifact':str(store),'record_sha256':sha(record),'dirty_session_guard':True})
    manifest['files']['INITRD.IMG']={'bytes':(out/'INITRD.IMG').stat().st_size,'sha256':sha(out/'INITRD.IMG')}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('WRITABLE_DESKTOP_PAIR_PREPARED',out,flush=True)


if __name__=='__main__':main()

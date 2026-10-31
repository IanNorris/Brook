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
    p.add_argument('--tcp-egress-1200',action='store_true',help='Enable a temporary 1200-byte TCP payload ceiling for a known path-MTU problem')
    p.add_argument('--ahci-observe',action='store_true',help='Opt in to the boot-time read-only AHCI register snapshot')
    p.add_argument('--ahci-read-only',action='store_true',help='Enable experimental read-only SATA inventory (requires --ahci-observe)')
    p.add_argument('--ahci-export',action='store_true',help='Opt in to root-only read-only AHCI export (requires --ahci-read-only)')
    p.add_argument('--dell-audio-dsdt',type=Path,help='Opt in to the reviewed Dell audio NVS snapshot for this exact DSDT')
    p.add_argument('--dell-i2c-id-probe',action='store_true',help='Opt in to one explicitly triggered Dell I2C0 identification read')
    p.add_argument('--build',type=Path,default=ROOT/'build/release')
    p.add_argument('--output',required=True,type=Path)
    a=p.parse_args()
    if a.ahci_read_only and not a.ahci_observe:p.error('--ahci-read-only requires --ahci-observe')
    if a.ahci_export and not a.ahci_read_only:p.error('--ahci-export requires --ahci-read-only')
    if a.dell_i2c_id_probe and not a.dell_audio_dsdt:p.error('--dell-i2c-id-probe requires --dell-audio-dsdt')
    store=a.store.resolve(strict=True);out=a.output.absolute()
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
    if a.tcp_egress_1200:
        marker=out/'tcp-egress-1200';marker.write_text('1\n')
        subprocess.run(['mcopy','-o','-i',str(out/'INITRD.IMG'),str(marker),'::ETC/tcp-egress-1200'],check=True,cwd=ROOT)
    if a.ahci_observe:
        marker=out/'ahci-observe';marker.write_text('1\n')
        subprocess.run(['mcopy','-o','-i',str(out/'INITRD.IMG'),str(marker),'::ETC/ahci-observe'],check=True,cwd=ROOT)
    if a.ahci_read_only:
        marker=out/'ahci-read-only';marker.write_text('1\n')
        subprocess.run(['mcopy','-o','-i',str(out/'INITRD.IMG'),str(marker),'::ETC/ahci-read-only'],check=True,cwd=ROOT)
    if a.ahci_export:
        marker=out/'ahci-export';marker.write_text('1\n')
        subprocess.run(['mcopy','-o','-i',str(out/'INITRD.IMG'),str(marker),'::ETC/ahci-export'],check=True,cwd=ROOT)
    audio_digest=None
    if a.dell_audio_dsdt:
        if a.dell_audio_dsdt.is_symlink() or not a.dell_audio_dsdt.is_file():p.error('DSDT must be a regular file')
        table=a.dell_audio_dsdt.read_bytes()
        if not 36<=len(table)<=1024*1024 or table[:4]!=b'DSDT' or int.from_bytes(table[4:8],'little')!=len(table) or sum(table)&255:
            p.error('DSDT must have a bounded valid header and checksum')
        audio_digest=hashlib.sha256(table).hexdigest();marker=out/'dell-audio-dsdt.sha256'
        marker.write_text(audio_digest+'\n');marker.chmod(0o600)
        subprocess.run(['mcopy','-o','-i',str(out/'INITRD.IMG'),str(marker),'::ETC/dell-audio-dsdt.sha256'],check=True,cwd=ROOT)
    if a.dell_i2c_id_probe:
        marker=out/'dell-i2c-id-probe';marker.write_text('1\n')
        subprocess.run(['mcopy','-o','-i',str(out/'INITRD.IMG'),str(marker),'::ETC/dell-i2c-id-probe'],check=True,cwd=ROOT)
    manifest=json.loads((out/'manifest.json').read_text())
    manifest.update(scope='Writable desktop preferred; accepted immutable RAM fallback',
        ephemeral_home=False,mutable_usb_application_mount=True,tcp_egress_ceiling=1200 if a.tcp_egress_1200 else 1460,
        ahci_observe=a.ahci_observe,ahci_read_only=a.ahci_read_only,ahci_export=a.ahci_export,
        dell_audio_dsdt_sha256=audio_digest,
        dell_i2c_id_probe=a.dell_i2c_id_probe,
        home_persistence='persistent only after writable-store admission; RAM fallback remains volatile',
        writable_desktop={'source':'/media/usb/STORE.IMG','readiness':'/media/usb/STORE.OK',
            'bytes':m['bytes'],'uuid':m['uuid'],'initial_sha256':m['sha256'],
            'store_artifact':str(store),'record_sha256':sha(record),'dirty_session_guard':True})
    manifest['files']['INITRD.IMG']={'bytes':(out/'INITRD.IMG').stat().st_size,'sha256':sha(out/'INITRD.IMG')}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('WRITABLE_DESKTOP_PAIR_PREPARED',out,flush=True)


if __name__=='__main__':main()

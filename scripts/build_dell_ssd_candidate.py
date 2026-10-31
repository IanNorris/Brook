#!/usr/bin/env python3
"""Create a private regular-file Dell SSD boot candidate; never opens disks.

The store is copied from a separately checked, clean snapshot. This produces
an image for later packing and verification; it does not install anything.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
SECTORS, FIRST, COUNT = 500118192, 2048, 12582912


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True, cwd=ROOT, stdout=subprocess.DEVNULL)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('desktop', 'recovery', 'store', 'store-marker', 'controller', 'disk-identity', 'kernel', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    a = parser.parse_args()
    controller = json.loads(a.controller.read_text())
    identity = json.loads(a.disk_identity.read_text())
    if ((controller['vendor'], controller['device'], controller['bus'], controller['slot'], controller['function']) !=
        (0x8086, 0x9c83, 0, 31, 2) or not controller['valid'] or
        (identity['port'], identity['sectors'], identity['logical_sector_bytes'], identity['physical_sector_bytes']) !=
        (3, SECTORS, 512, 512) or not identity['ready'] or identity['quarantined']):
        raise ValueError('observed Dell controller or disk identity does not match')
    for value in (identity['model'], identity['serial']):
        if not value or '\n' in value or '\r' in value:
            raise ValueError('invalid ATA identity')
    for path in (a.store, a.kernel, a.desktop / 'INITRD.IMG', a.recovery / 'payload/APPS.BRK'):
        if path.is_symlink() or not path.is_file():
            raise ValueError('inputs must be regular files')
    if a.store.stat().st_size != 4026531840:
        raise ValueError('unexpected store size')
    out = a.output.absolute()
    out.mkdir(parents=True, exist_ok=False, mode=0o700)
    disk, initrd = out / 'ssd.raw', out / 'INITRD.IMG'
    with disk.open('xb') as stream:
        stream.truncate(SECTORS * 512)
    dg, pg = uuid.uuid4(), uuid.uuid4()
    layout = f'label: gpt\nlabel-id: {dg}\nunit: sectors\n\nstart={FIRST},size={COUNT},type=U,uuid={pg}\n'
    subprocess.run(['sfdisk', str(disk)], input=layout, text=True, check=True, stdout=subprocess.DEVNULL)
    run('mkfs.fat', '-F', '32', '--offset', FIRST, disk, COUNT // 2)
    media = str(disk) + '@@' + str(FIRST * 512)
    shutil.copyfile(a.desktop / 'INITRD.IMG', initrd)
    run('mmd', '-i', initrd, '::MEDIA/SYSTEM')
    # Installed kernel may write only the exact observed drive. The installer
    # kernel and its authorization never become part of the installed system.
    target = out / 'write-target'
    target.write_text(f'BRKAHCI1\n{controller["vendor"]},{controller["device"]},{controller["bus"]},'
                      f'{controller["slot"]},{controller["function"]},{identity["port"]},{SECTORS},512,512\n'
                      f'{identity["model"]}\n{identity["serial"]}\n')
    os.chmod(target, 0o600)
    selector = out / 'boot-volume'
    selector.write_text(f'BRKESP1\n{dg.bytes_le.hex()}\n{pg.bytes_le.hex()}\n{FIRST},{COUNT}\n')
    apps = a.recovery / 'payload/APPS.BRK'
    appconfig = out / 'boot-app-snapshot'
    appconfig.write_text(f'/media/system/APPS.BRK /nix {apps.stat().st_size} {sha(apps)}\n')
    for source, name in ((target, 'ahci-write-target'), (selector, 'ahci-boot-volume'),
                         (appconfig, 'boot-app-snapshot')):
        run('mcopy', '-o', '-i', initrd, source, '::ETC/' + name)
    run('mcopy', '-s', '-i', media, a.recovery / 'payload/EFI', '::/')
    run('mmd', '-i', media, '::KERNEL')
    cfg = out / 'BROOK.CFG'
    cfg.write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nDISPLAY_WIDTH=3200\nDISPLAY_HEIGHT=1800\n')
    for source, name in ((a.kernel, 'KERNEL/BROOK.ELF'), (initrd, 'INITRD.IMG'),
                         (cfg, 'BROOK.CFG'), (apps, 'APPS.BRK'),
                         (a.store, 'STORE.IMG'), (a.store_marker, 'STORE.OK')):
        run('mcopy', '-i', media, source, '::' + name)
    receipt = {'scope': 'regular-file Dell SSD candidate; no physical writes',
               'disk_sectors': SECTORS, 'partition_first': FIRST, 'partition_sectors': COUNT,
               'disk_guid': str(dg), 'partition_guid': str(pg), 'store_sha256': sha(a.store),
               'kernel_sha256': sha(a.kernel), 'initrd_sha256': sha(initrd), 'apps_sha256': sha(apps)}
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('DELL_SSD_CANDIDATE_READY', out, flush=True)


if __name__ == '__main__':
    main()

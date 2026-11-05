#!/usr/bin/env python3
"""Boot the one-time firmware inventory on a private QEMU disk."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_recovery_menu_boot import boot
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from efi_boot_inventory import parse_inventory


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--efi',type=Path,required=True)
    p.add_argument('--firmware',type=Path,required=True)
    a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='brook-boot-inventory-') as tmp:
        root=Path(tmp);disk=root/'disk.raw';shutil.copyfile(a.image.resolve(strict=True),disk)
        media=f'{disk}@@1048576'
        subprocess.run(['mcopy','-o','-i',media,str(a.efi.resolve(strict=True)),'::/EFI/BOOT/BOOTX64.EFI'],check=True)
        for attempt in range(2):
            work=root/f'boot-{attempt}';work.mkdir()
            boot(disk,a.firmware.resolve(strict=True),work,False,'BOOT: complete',90,True,sata_snapshot=False)
            data=subprocess.check_output(['mtype','-i',media,'::/RECOVERY/BOOTVARS.TXT'])
            if attempt and data!=first:raise RuntimeError('inventory file was overwritten')
            first=data
        lines=data.decode('ascii').splitlines()
        if lines[0]!='BRKBOOTV1' or lines[-1]!='END':raise RuntimeError('incomplete inventory')
        rows={}
        for line in lines[1:-1]:
            name,sep,value=line.partition('=')
            if not sep or name in rows or not re.fullmatch(r'(Boot[0-9A-F]{4}|BootOrder|BootCurrent|BootNext)',name):
                raise RuntimeError('invalid inventory entry')
            rows[name]=bytes.fromhex(value)
        if len(rows.get('BootCurrent',b''))!=2 or not rows.get('BootOrder') or len(rows['BootOrder'])%2:
            raise RuntimeError('boot state missing or malformed')
        current=int.from_bytes(rows['BootCurrent'],'little')
        if f'Boot{current:04X}' not in rows:raise RuntimeError('current boot option absent')
        parsed=parse_inventory(data)
        if parsed['boot_current']!=f'Boot{current:04X}' or parsed['missing_or_unparsed_order_entries']:
            raise RuntimeError('host decoder could not account for firmware boot order')
        print('UEFI boot inventory and normal boot: PASS')


if __name__=='__main__':main()

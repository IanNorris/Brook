#!/usr/bin/env python3
"""Exercise alternate EFI recovery and legacy fallback on private QEMU disks."""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

from test_guard_repair_handoff_qemu import compact
from test_recovery_menu_boot import boot
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_update_protocol import record


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','efi','firmware','alternate-pair','output'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='brk-alt-recovery-') as tmp:
        root=Path(tmp);initrd=root/'alternate.img'
        compact(a.alternate_pair/'INITRD.IMG',initrd,root/'tree')
        kernel=(a.alternate_pair/'KERNEL.ELF').read_bytes()
        kernel_file=root/'KERNEL.ELF';kernel_file.write_bytes(kernel)
        selector=root/'SELECT.BIN';selector.write_bytes(record(1,0,kernel,initrd.read_bytes()))
        for case in ('valid','missing-selector','corrupt-initrd','both-unavailable'):
            work=root/case;work.mkdir();disk=work/'disk.raw';shutil.copyfile(a.image,disk)
            media=str(disk)+'@@1048576'
            subprocess.run(['mcopy','-o','-i',media,str(a.efi),'::/EFI/BOOT/BOOTX64.EFI'],check=True)
            subprocess.run(['mmd','-i',media,'::/RECOV2'],check=True)
            for src,name in ((kernel_file,'KERNEL.ELF'),(initrd,'INITRD.IMG')):
                subprocess.run(['mcopy','-o','-i',media,str(src),'::/RECOV2/'+name],check=True)
            if case not in ('missing-selector','both-unavailable'):
                subprocess.run(['mcopy','-o','-i',media,str(selector),'::/RECOV2/SELECT.BIN'],check=True)
            if case=='corrupt-initrd':
                damaged=work/'damaged.img';data=bytearray(initrd.read_bytes());data[-1]^=1;damaged.write_bytes(data)
                subprocess.run(['mcopy','-o','-i',media,str(damaged),'::/RECOV2/INITRD.IMG'],check=True)
            if case=='both-unavailable':
                subprocess.run(['mdel','-i',media,'::/RECOVERY/SELECT.BIN'],check=True)
                try:
                    boot(disk,a.firmware,work,True,'Brook kernel starting...',15,True,
                         sata_serial='BROOK-RECOVERY-QA',forbidden='Brook kernel starting...')
                except RuntimeError as exc:
                    if 'timed out waiting for' not in str(exc):raise
                else:raise RuntimeError('booted with both recovery pairs unavailable')
                shutil.copyfile(work/'serial.log',a.output/(case+'.log'))
                print(case+': PASS (no kernel boot during observation)',flush=True)
                continue
            marker='SSD_GUARD_RESERVATION_READY' if case=='valid' else 'RECOVERY_READONLY_CHECK_BEGIN'
            boot(disk,a.firmware,work,True,marker,90,True,sata_serial='BROOK-RECOVERY-QA')
            text=(work/'serial.log').read_text(errors='replace')
            other='RECOVERY_READONLY_CHECK_BEGIN' if case=='valid' else 'SSD_GUARD_RESERVATION_READY'
            if other in text:raise RuntimeError('wrong recovery pair selected: '+case)
            shutil.copyfile(work/'serial.log',a.output/(case+'.log'))
            print(case+': PASS',flush=True)


if __name__=='__main__':main()

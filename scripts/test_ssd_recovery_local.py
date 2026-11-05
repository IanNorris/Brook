#!/usr/bin/env python3
"""Boot a disposable SATA recovery image without network or USB storage.

The fixture has its own QEMU-only disk identity. It is copied before changes;
the complete copied disk must remain byte-identical during the guest check.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_update_protocol import record
from hardware_recovery_fsck import validate_checker
from prepare_hardware_kernel_pair import module_version
from prepare_ssd_recovery_pair import AUTO_CHECK_INIT, AUTO_CHECK_PAYLOAD_ID
from test_recovery_menu_boot import boot as boot_menu_fixture


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', required=True, type=Path)
    p.add_argument('--supervisor', required=True, type=Path)
    p.add_argument('--firmware', required=True, type=Path)
    p.add_argument('--dirty', action='store_true',
                   help='Introduce a bitmap inconsistency only in the private fixture copy')
    p.add_argument('--missing-checker', action='store_true',
                   help='Remove the checker from the private initrd to test failure handling')
    p.add_argument('--frame-output', type=Path,
                   help='Optionally save the final QEMU frame as a new PPM file')
    p.add_argument('--serial-output', type=Path,
                   help='Optionally retain the test serial log as a new file')
    p.add_argument('--kernel-build', type=Path,
                   help='Optionally stage a private installer kernel and matching ABI 64 modules')
    p.add_argument('--prepared-pair', type=Path,
                   help='Boot this exact physical recovery pair and verify it refuses the QEMU disk')
    p.add_argument('--fat-checker', type=Path,
                   help='Use a static FAT checker and matching diagnostic supervisor on the private ESP')
    p.add_argument('--fat-crosslink', action='store_true',
                   help='Crosslink two new files in the private FAT fixture before boot')
    p.add_argument('--guard-open-probe', action='store_true',
                   help='Use a guard-open probe as supervisor on the private fixture')
    p.add_argument('--guard-reservation', choices=('valid', 'wrong-guid', 'wrong-fs-uuid'),
                   help='Configure the disposable guard-only kernel reservation fixture')
    p.add_argument('--guard-poison-boot', action='store_true',
                   help='Attach a disposable virtio disk requesting an overmount of /boot')
    p.add_argument('--guard-real-checker', action='store_true',
                   help='Run the kernel-started, pinned FAT checker on the reserved ESP')
    p.add_argument('--guard-bad-checker', action='store_true',
                   help='Corrupt one byte of the private checker to test digest rejection')
    p.add_argument('--guard-records', choices=('valid', 'mismatch', 'wrong-identity', 'missing', 'oversized'),
                   help='Exercise read-only private guard record inspection')
    p.add_argument('--guard-pending-test', action='store_true',
                   help='Run a writable pending transaction only on this private QEMU disk')
    p.add_argument('--guard-pending-no-marker', action='store_true',
                   help='Verify the pending test refuses its disk without the test marker')
    p.add_argument('--guard-pending-wrong-serial', action='store_true',
                   help='Verify the pending test refuses a differently identified disk')
    p.add_argument('--guard-pending-fail-at', type=int, choices=range(1,12),
                   help='Inject a failure at a specific pending transaction operation')
    p.add_argument('--guarded-efi', type=Path,
                   help='Verify a fixture-bound EFI loader accepts clean guards and rejects pending')
    p.add_argument('--guard-ext2-test', action='store_true',
                   help='Run guarded ext2 repair and verification on the private data partition')
    p.add_argument('--guard-complete-test', action='store_true',
                   help='Complete verified recovery on the private QEMU disk')
    p.add_argument('--guard-auto-restart', action='store_true',
                   help='Exercise a guest-initiated restart after verified completion')
    p.add_argument('--guard-complete-fail-at', type=int, choices=range(1,12),
                   help='Inject a failure in the guard completion transaction')
    p.add_argument('--guard-desktop-kernel-build', type=Path,
                   help='Also boot a normal kernel and read the repaired filesystem')
    p.add_argument('--guard-desktop-probe', type=Path,
                   help='Static recovery_mount_check workload for normal boot')
    p.add_argument('--guard-ext2-bad-checker', action='store_true',
                   help='Alter the private ext2 checker to test digest rejection')
    p.add_argument('--guard-ext2-missing-checker', action='store_true',
                   help='Remove the private ext2 checker to test missing-tool handling')
    a = p.parse_args()
    if a.guard_auto_restart and not a.guard_complete_test:
        p.error('automatic restart requires the completion fixture')
    if a.dirty and a.missing_checker:
        p.error('choose one failure scenario')
    if a.prepared_pair and (a.dirty or a.missing_checker or a.kernel_build or a.fat_checker):
        p.error('prepared pair is an independent identity rejection scenario')
    if a.fat_checker and ((a.dirty and not a.guard_ext2_test) or a.missing_checker):
        p.error('FAT checker uses a clean fixture')
    if a.fat_crosslink and not a.fat_checker:
        p.error('--fat-crosslink requires --fat-checker')
    if a.guard_open_probe and (a.dirty or a.missing_checker or a.fat_checker or a.prepared_pair):
        p.error('guard open probe is an independent scenario')
    if a.guard_real_checker and (not a.fat_checker or a.guard_open_probe):
        p.error('guard real checker requires --fat-checker and excludes the open probe')
    if a.guard_bad_checker and (not a.guard_real_checker or a.fat_crosslink):
        p.error('bad checker test requires a real checker and excludes crosslink')
    if a.guard_records and (not a.guard_real_checker or a.guard_reservation != 'valid' or
                            a.guard_bad_checker or a.fat_crosslink):
        p.error('guard record test requires the clean checker and valid reservation')
    if a.guard_pending_test and not a.guard_records:
        p.error('pending transaction test requires private guard records')
    if (a.guard_pending_no_marker or a.guard_pending_wrong_serial) and not a.guard_pending_test:
        p.error('pending test refusal cases require --guard-pending-test')
    if a.guard_pending_no_marker and a.guard_pending_wrong_serial:
        p.error('choose one pending test admission failure')
    if a.guard_pending_fail_at and (not a.guard_pending_test or a.guard_records!='valid' or
                                    a.guard_pending_no_marker or a.guard_pending_wrong_serial):
        p.error('pending fault injection requires the admitted valid-record case')
    if a.guarded_efi and (not a.guard_pending_test or a.guard_records!='valid' or
                          a.guard_pending_no_marker or a.guard_pending_wrong_serial):
        p.error('guarded EFI test requires the successful pending scenario')
    if a.guard_ext2_test and (not a.guard_pending_test or not a.guarded_efi or a.guard_records!='valid'):
        p.error('guarded ext2 test requires the successful guarded pending fixture')
    if a.guard_complete_test and (not a.guard_ext2_test or a.guard_pending_fail_at or
                                   a.guard_ext2_bad_checker or a.guard_ext2_missing_checker):
        p.error('completion requires an uninjected guarded ext2 repair')
    if a.guard_complete_fail_at and not a.guard_complete_test:
        p.error('completion fault injection requires the completion test')
    if bool(a.guard_desktop_kernel_build)!=bool(a.guard_desktop_probe) or (
            a.guard_desktop_kernel_build and (not a.guard_complete_test or a.guard_complete_fail_at)):
        p.error('desktop verification needs a normal kernel, probe and uninjected completion')
    if (a.guard_ext2_bad_checker or a.guard_ext2_missing_checker) and (not a.guard_ext2_test or a.guard_pending_fail_at):
        p.error('ext2 checker failure requires an uninjected guarded ext2 run')
    if a.guard_ext2_bad_checker and a.guard_ext2_missing_checker:
        p.error('choose one ext2 checker failure')
    if a.guard_reservation and (not (a.guard_open_probe or a.guard_real_checker) or not a.kernel_build):
        p.error('guard reservation requires a guard-only kernel and a checker or open probe')
    if a.guard_real_checker and not a.guard_reservation:
        p.error('guard real checker requires a guard reservation')
    if a.guard_poison_boot and a.guard_reservation != 'valid':
        p.error('guard poison disk requires the valid reservation fixture')
    image = a.image.resolve(strict=True)
    if not image.is_file() or image.stat().st_size != 128 * 1024 * 1024:
        p.error('expected the 128 MiB disposable SATA recovery fixture')
    table = json.loads(subprocess.check_output(['sfdisk', '--json', str(image)]))['partitiontable']
    parts = table.get('partitions', [])
    if (table.get('label') != 'gpt' or table.get('sectorsize') != 512 or len(parts) != 2 or
            parts[0].get('start') != 2048 or parts[0].get('size') != 131072 or
            parts[0].get('type', '').lower() != 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b' or
            parts[1].get('start') != 133120 or parts[1].get('size') != 126976 or
            parts[1].get('type', '').lower() != '0fc63daf-8483-4772-8e79-3d69d8477de4'):
        p.error('unexpected fixture partition geometry')
    validate_checker(a.supervisor)
    fat_info=validate_checker(a.fat_checker) if a.fat_checker else None
    with tempfile.TemporaryDirectory(prefix='brook-recovery-local-') as directory:
        work = Path(directory)
        disk = work / 'disk.raw'
        shutil.copyfile(image, disk)
        if a.dirty or a.guard_ext2_test:
            partition = work / 'partition.img'
            subprocess.run(['dd', f'if={disk}', f'of={partition}', 'bs=512',
                            'skip=133120', 'count=126976', 'status=none'], check=True)
            if a.guard_desktop_kernel_build:
                with partition.open('rb') as stream:
                    stream.seek(1024+104); original_uuid=stream.read(16)
                if len(original_uuid)!=16 or not any(original_uuid):
                    raise RuntimeError('normal-mount fixture lacks its original UUID')
                subprocess.run(['mke2fs','-q','-F','-t','ext2','-b','4096','-I','128','-N','1024',
                                '-m','0','-O','none,filetype,sparse_super,large_file',
                                '-U',str(uuid.UUID(bytes=original_uuid)),str(partition)],check=True,
                               stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            if a.guard_ext2_test:
                known=work/'known.txt';known.write_bytes(b'Brook guarded recovery fixture\n')
                subprocess.run(['debugfs','-w','-R',f'write {known} /brook-recovery-known',str(partition)],
                               check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                actual=subprocess.check_output(['debugfs','-R','cat /brook-recovery-known',str(partition)],stderr=subprocess.DEVNULL)
                if actual!=known.read_bytes():raise RuntimeError('known file was not created')
            if a.guard_desktop_kernel_build:
                # Minimal, explicitly synthetic desktop tree on the private
                # ext2 image. The normal kernel still runs real admission.
                dirs=('store','profile','profile/bin','profile/lib','profile/share',
                      'profile/share/applications','home','home/brook','var','var/cache',
                      'var/cache/narinfo','share','share/applications','index','bin')
                for name in dirs:
                    for command in (f'mkdir /{name}', f'set_inode_field /{name} uid 1000',
                                    f'set_inode_field /{name} gid 1000'):
                        subprocess.run(['debugfs','-w','-R',command,str(partition)],check=True,
                                       stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                for name in ('profile/manifest.tsv','index/packages.idx','share/applications/applications.idx',
                             'bin/nix','bin/nix-fetch','bin/nar-unpack','profile/bin/brook-firefox',
                             'profile/bin/brook-chromium','profile/bin/brook-gallery'):
                    subprocess.run(['debugfs','-w','-R',f'write {known} /{name}',str(partition)],check=True,
                                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    if name.startswith(('bin/','profile/bin/')):
                        subprocess.run(['debugfs','-w','-R',f'set_inode_field /{name} mode 0100755',str(partition)],
                                       check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            if a.dirty:
                for command in ('setb 10000', 'set_super_value state 0'):
                    subprocess.run(['debugfs', '-w', '-R', command, str(partition)], check=True,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                result = subprocess.run(['e2fsck', '-f', '-n', str(partition)],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if result.returncode != 4 or b'bitmap differences' not in result.stdout.lower():
                    raise RuntimeError('fixture did not develop the expected bitmap inconsistency')
            subprocess.run(['dd', f'if={partition}', f'of={disk}', 'bs=512',
                            'seek=133120', 'conv=notrunc', 'status=none'], check=True)
        media = str(disk) + '@@1048576'
        initrd = work / 'initrd.img'
        subprocess.run(['mcopy', '-i', media, '::/RECOVERY/INITRD.IMG', str(initrd)], check=True)
        if a.guard_ext2_missing_checker:
            subprocess.run(['mdel','-i',str(initrd),'::/BOOT/E2FSCK.ELF'],check=True)
        if a.guard_ext2_bad_checker:
            checker_file=work/'bad-e2fsck.elf'
            data=bytearray(subprocess.check_output(['mtype','-i',str(initrd),'::/BOOT/E2FSCK.ELF']))
            data[-1]^=1;checker_file.write_bytes(data)
            subprocess.run(['mcopy','-o','-i',str(initrd),str(checker_file),'::/BOOT/E2FSCK.ELF'],check=True)
        if a.missing_checker:
            subprocess.run(['mdel', '-i', str(initrd), '::/BOOT/E2FSCK.ELF'], check=True)
        kernel = subprocess.check_output(['mtype', '-i', media, '::/RECOVERY/KERNEL.ELF'])
        if a.prepared_pair:
            pair = a.prepared_pair.resolve(strict=True)
            kernel = (pair / 'KERNEL.ELF').read_bytes()
            initrd.write_bytes((pair / 'INITRD.IMG').read_bytes())
            kernel_file = work / 'KERNEL.ELF'; kernel_file.write_bytes(kernel)
            subprocess.run(['mcopy', '-o', '-i', media, str(kernel_file), '::/RECOVERY/KERNEL.ELF'], check=True)
        if a.kernel_build:
            build = a.kernel_build.resolve(strict=True)
            kernel = (build / 'BROOK.elf').read_bytes()
            kernel_file = work / 'KERNEL.ELF'; kernel_file.write_bytes(kernel)
            subprocess.run(['mcopy', '-o', '-i', media, str(kernel_file), '::/RECOVERY/KERNEL.ELF'], check=True)
            listing = subprocess.check_output(['mdir', '-a', '-b', '-i', str(initrd),
                                               '::/DRIVERS/*'], text=True)
            names = [line.rsplit('/', 1)[-1] for line in listing.splitlines() if line]
            if not names:
                raise RuntimeError('recovery initrd has no driver modules')
            for name in names:
                if not name.lower().endswith('.mod') or '/' in name or '..' in name:
                    raise RuntimeError('unexpected recovery module name')
                module = build / 'drivers' / name.lower()
                data = module.read_bytes()
                if module_version(data) != 64:
                    raise RuntimeError('unexpected recovery module ABI')
                subprocess.run(['mcopy', '-o', '-i', str(initrd), str(module), '::/DRIVERS/' + name], check=True)
                if subprocess.check_output(['mtype', '-i', str(initrd), '::/DRIVERS/' + name]) != data:
                    raise RuntimeError('recovery module readback mismatch')
        if a.guard_reservation:
            # Private disk identity and selectors; never copy these into a real image.
            with disk.open('rb') as stream:
                stream.seek(parts[1]['start'] * 512 + 1024 + 104)
                ext2_uuid = stream.read(16)
            if len(ext2_uuid) != 16 or not any(ext2_uuid):
                raise RuntimeError('fixture ext2 UUID is absent')
            if a.guard_reservation == 'wrong-fs-uuid':
                ext2_uuid = bytes([ext2_uuid[0] ^ 1]) + ext2_uuid[1:]
            disk_guid = uuid.UUID(table['id']).bytes_le.hex()
            esp_guid = uuid.UUID(parts[0]['uuid']).bytes_le.hex()
            if a.guard_reservation == 'wrong-guid':
                esp_guid = ('0' if esp_guid[0] != '0' else '1') + esp_guid[1:]
            data_guid = uuid.UUID(parts[1]['uuid']).bytes_le.hex()
            records = {
                'ahci-write-target': 'BRKAHCI1\n32902,10530,0,31,2,0,262144,512,512\nQEMU HARDDISK\n' +
                                     ('BROOK-WRONG-ID\n' if a.guard_pending_wrong_serial else 'BROOK-RECOVERY-QA\n'),
                'ahci-boot-volume': f'BRKESP1\n{disk_guid}\n{esp_guid}\n2048,131072\n',
                'ahci-data-volume': f'BRKDATA1\n{disk_guid}\n{data_guid}\n133120,126976\n{ext2_uuid.hex()}\n',
            }
            for name in ('ssd-install-mode', 'ssd-block-tools', 'boot-app-image', 'boot-app-snapshot', 'writable-desktop'):
                subprocess.run(['mdel', '-i', str(initrd), '::ETC/' + name],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for name, content in records.items():
                record_file = work / name
                record_file.write_text(content)
                subprocess.run(['mcopy', '-o', '-i', str(initrd), str(record_file), '::ETC/' + name], check=True)
                if subprocess.check_output(['mtype', '-i', str(initrd), '::ETC/' + name]) != content.encode():
                    raise RuntimeError('guard reservation record readback mismatch: ' + name)
            if a.guard_desktop_kernel_build:
                normal_initrd=work/'normal-initrd.img'
                subprocess.run(['mcopy','-i',media,'::/INITRD.IMG',str(normal_initrd)],check=True)
                for directory in ('MEDIA','MEDIA/SYSTEM','NIX','DATA'):
                    subprocess.run(['mmd','-i',str(normal_initrd),'::/'+directory],
                                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    subprocess.run(['mdir','-i',str(normal_initrd),'::/'+directory],check=True,
                                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                for marker in ('ahci-observe','ahci-read-only','volatile-desktop'):
                    ahci_marker=work/('normal-'+marker);ahci_marker.write_bytes(b'1\n')
                    subprocess.run(['mcopy','-o','-i',str(normal_initrd),str(ahci_marker),'::ETC/'+marker],check=True)
                for name,content in records.items():
                    item=work/('normal-'+name);item.write_text(content)
                    subprocess.run(['mcopy','-o','-i',str(normal_initrd),str(item),'::ETC/'+name],check=True)
                normal_init=work/'normal-init.rc'
                normal_init.write_bytes(b'set wm\nset vfb none\nsched eevdf\nrun --console --uid 0 /boot/RCVMOUNT.ELF\n')
                subprocess.run(['mcopy','-o','-i',str(normal_initrd),str(normal_init),'::BOOT/INIT.RC'],check=True)
                subprocess.run(['mcopy','-o','-i',str(normal_initrd),str(a.guard_desktop_probe.resolve(strict=True)),
                                '::BOOT/RCVMOUNT.ELF'],check=True)
                normal_build=a.guard_desktop_kernel_build.resolve(strict=True)
                listing=subprocess.check_output(['mdir','-a','-b','-i',str(normal_initrd),'::/DRIVERS/*'],text=True)
                for line in listing.splitlines():
                    name=line.rsplit('/',1)[-1]
                    if not name.lower().endswith('.mod') or '/' in name or '..' in name:
                        raise RuntimeError('unexpected normal module name')
                    module=normal_build/'drivers'/name.lower()
                    if module_version(module.read_bytes())!=64:raise RuntimeError('unexpected normal module ABI')
                    subprocess.run(['mcopy','-o','-i',str(normal_initrd),str(module),'::DRIVERS/'+name],check=True)
                subprocess.run(['mcopy','-o','-i',media,str(normal_initrd),'::/INITRD.IMG'],check=True)
                subprocess.run(['mcopy','-o','-i',media,str(normal_build/'BROOK.elf'),'::/KERNEL/BROOK.ELF'],check=True)
            if a.guard_pending_test and not a.guard_pending_no_marker:
                marker=work/'guard-pending-test';marker.write_bytes(b'1\n')
                subprocess.run(['mcopy','-o','-i',str(initrd),str(marker),'::ETC/guard-pending-test'],check=True)
            elif a.guard_pending_no_marker:
                subprocess.run(['mdel','-i',str(initrd),'::ETC/guard-pending-test'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                if subprocess.run(['mtype','-i',str(initrd),'::ETC/guard-pending-test'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0:
                    raise RuntimeError('pending marker unexpectedly present')
        init = work / 'INIT.RC'; init.write_bytes(AUTO_CHECK_INIT)
        ident = work / 'hardware-payload-id'; ident.write_bytes(AUTO_CHECK_PAYLOAD_ID)
        if not a.prepared_pair:
            for source, target in ((init, '::/BOOT/INIT.RC'),
                                   (ident, '::/ETC/hardware-payload-id'),
                                   (a.supervisor, '::/BOOT/RECCHECK.ELF')):
                subprocess.run(['mcopy', '-o', '-i', str(initrd), str(source), target], check=True)
            if a.fat_checker:
                checker = a.fat_checker
                if a.guard_bad_checker:
                    checker = work / 'bad-checker.elf'
                    data = bytearray(a.fat_checker.read_bytes()); data[-1] ^= 1
                    checker.write_bytes(data)
                subprocess.run(['mcopy', '-o', '-i', str(initrd), str(checker), '::/BOOT/FATFSCK.ELF'], check=True)
                copied=subprocess.check_output(['mtype','-i',str(initrd),'::/BOOT/FATFSCK.ELF'])
                expected_hash = sha(checker) if a.guard_bad_checker else fat_info['sha256']
                if len(copied)!=fat_info['bytes'] or hashlib.sha256(copied).hexdigest()!=expected_hash:
                    raise RuntimeError('guest FAT checker readback mismatch')
        selection = work / 'SELECT.BIN'; selection.write_bytes(record(1, 0, kernel, initrd.read_bytes()))
        if a.prepared_pair and selection.read_bytes() != (pair / 'SELECT.BIN').read_bytes():
            raise RuntimeError('prepared recovery selection does not match its exact pair')
        for source, target in ((initrd, '::/RECOVERY/INITRD.IMG'),
                               (selection, '::/RECOVERY/SELECT.BIN')):
            subprocess.run(['mcopy', '-o', '-i', media, str(source), target], check=True)
        if a.prepared_pair:
            for name in ('KERNEL.ELF', 'INITRD.IMG', 'SELECT.BIN'):
                staged = subprocess.check_output(['mtype', '-i', media, '::/RECOVERY/' + name])
                if staged != (pair / name).read_bytes():
                    raise RuntimeError('prepared recovery pair readback mismatch: ' + name)
        if a.fat_crosslink:
            payload=work/'fat-crosslink.bin';payload.write_bytes(bytes(range(128)))
            for name in ('QA0.BIN','QA1.BIN'):
                subprocess.run(['mcopy','-i',media,str(payload),'::/'+name],check=True)
            with disk.open('r+b') as stream:
                stream.seek(1048576);header=stream.read(512)
                sector=struct.unpack_from('<H',header,11)[0];reserved=struct.unpack_from('<H',header,14)[0]
                fats=header[16];root_count=struct.unpack_from('<H',header,17)[0]
                fat_size=struct.unpack_from('<H',header,22)[0] or struct.unpack_from('<I',header,36)[0]
                if sector!=512 or not reserved or not fats or not fat_size:
                    raise RuntimeError('unexpected FAT crosslink fixture geometry')
                data=reserved+fats*fat_size+(root_count*32+511)//512
                root=(reserved+fats*fat_size if root_count else
                      data+(struct.unpack_from('<I',header,44)[0]-2)*header[13])
                stream.seek(1048576+root*512);entries=bytearray(stream.read(512))
                a0=next(i for i in range(0,len(entries),32) if entries[i:i+11]==b'QA0     BIN')
                a1=next(i for i in range(0,len(entries),32) if entries[i:i+11]==b'QA1     BIN')
                cluster=lambda i: struct.unpack_from('<H',entries,i+26)[0] | (struct.unpack_from('<H',entries,i+20)[0]<<16)
                if cluster(a0)<2 or cluster(a1)<2 or cluster(a0)==cluster(a1):
                    raise RuntimeError('FAT crosslink source files were not distinct')
                for offset in (20,26):entries[a1+offset:a1+offset+2]=entries[a0+offset:a0+offset+2]
                stream.seek(1048576+root*512);stream.write(entries)
        if a.guard_records:
            with disk.open('rb') as stream:
                stream.seek(parts[1]['start']*512+1024+104); filesystem_uuid=stream.read(16)
            record_data=bytearray(128);record_data[:8]=b'BRKRCV01'
            struct.pack_into('<QI',record_data,8,1,1)
            record_data[24:40]=uuid.UUID(table['id']).bytes_le
            record_data[40:56]=uuid.UUID(parts[1]['uuid']).bytes_le
            record_data[56:72]=filesystem_uuid
            struct.pack_into('<I',record_data,124,zlib.crc32(record_data[:124]))
            for index in range(2):
                target=f'::/RECOVERY/GUARD{index}.BIN'
                if a.guard_records=='missing' and index==1:
                    subprocess.run(['mdel','-i',media,target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    continue
                contents=bytearray(record_data)
                if (a.guard_records=='mismatch' and index==1) or a.guard_records=='wrong-identity':
                    contents[40]^=1;struct.pack_into('<I',contents,124,zlib.crc32(contents[:124]))
                if a.guard_records=='oversized' and index==1:contents+=b'X'
                record_file=work/f'guard{index}.bin';record_file.write_bytes(contents)
                subprocess.run(['mcopy','-o','-i',media,str(record_file),target],check=True)
        if a.guarded_efi:
            efi=a.guarded_efi.resolve(strict=True)
            subprocess.run(['mcopy','-o','-i',media,str(efi),'::/EFI/BOOT/BOOTX64.EFI'],check=True)
            normal=work/'normal-boot';normal.mkdir()
            boot_menu_fixture(disk,a.firmware.resolve(strict=True),normal,False,'BOOT: complete',90,True,
                              'Recovery required; normal boot blocked.',sata_snapshot=True)
        before = sha(disk)
        baseline=None
        if a.guard_pending_test and a.guard_records=='valid' and not (a.guard_pending_no_marker or a.guard_pending_wrong_serial):
            baseline=work/'baseline.raw';shutil.copyfile(disk,baseline)
        poison = None
        if a.guard_poison_boot:
            poison = work / 'poison.raw'
            with poison.open('xb') as stream:
                stream.truncate(16 * 1024 * 1024)
            subprocess.run(['mkfs.fat', str(poison)], stdout=subprocess.DEVNULL, check=True)
            mount = work / 'BROOK.MNT'; mount.write_text('/boot')
            subprocess.run(['mcopy', '-i', str(poison), str(mount), '::BROOK.MNT'], check=True)
            poison_before = sha(poison)
        firmware = a.firmware.resolve(strict=True)
        shutil.copyfile(firmware / 'OVMF_VARS.fd', work / 'vars.fd')
        monitor = work / 'monitor.sock'; serial = work / 'serial.log'
        command = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host',
                   '-smp', '4', '-m', '2048', '-nodefaults',
                   '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
                   '-drive', f'if=pflash,format=raw,file={work / "vars.fd"}', '-device', 'VGA',
                   '-drive', f'if=none,id=sata,format=raw,file={disk}',
                   '-device', 'ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=' +
                              ('BROOK-WRONG-ID' if a.guard_pending_wrong_serial else 'BROOK-RECOVERY-QA'),
                   '-display', 'none', '-serial', f'file:{serial}',
                   '-monitor', f'unix:{monitor},server=on,wait=off']
        if a.guard_auto_restart:
            command += ['-fw_cfg', 'name=opt/guard-complete-restart,string=test-1']
        else:
            command += ['-no-reboot']
        if poison:
            command += ['-drive', f'if=none,id=poison,format=raw,file={poison}',
                        '-device', 'virtio-blk-pci,drive=poison']
        if a.guard_pending_fail_at:
            command += ['-fw_cfg', f'name=opt/guard-pending-fail,string={a.guard_pending_fail_at}']
        if a.guard_complete_fail_at:
            command += ['-fw_cfg', f'name=opt/guard-complete-fail,string={a.guard_complete_fail_at}']
        with (work / 'qemu.log').open('wb') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            stop = threading.Event()
            def select_recovery():
                while not stop.is_set() and process.poll() is None:
                    if monitor.exists():
                        try:
                            with socket.socket(socket.AF_UNIX) as connection:
                                connection.settimeout(1); connection.connect(str(monitor))
                                connection.sendall(b'sendkey f5\n')
                        except OSError:
                            pass
                    stop.wait(.2)
            thread = threading.Thread(target=select_recovery, daemon=True); thread.start()
            try:
                deadline = time.monotonic() + 120
                selected = False
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        detail = (work / 'qemu.log').read_text(errors='replace')[-4000:]
                        raise RuntimeError('QEMU exited before local check completed: ' + detail)
                    text = serial.read_text(errors='replace') if serial.exists() else ''
                    if (a.guard_reservation=='valid' and not (a.guard_pending_no_marker or a.guard_pending_wrong_serial) and
                            'SSD_GUARD_RESERVATION_FAILED' in text):
                        raise RuntimeError('guard reservation failed: '+text[-4000:])
                    if ((a.guard_reservation in ('wrong-guid', 'wrong-fs-uuid') or
                         a.guard_pending_no_marker or a.guard_pending_wrong_serial) and
                            'SSD guard reservation failed; no recovery access permitted' in text and
                            'System halted.' in text):
                        if 'SSD_GUARD_RESERVATION_READY' in text or 'ahci system: verified GPT entry' in text:
                            raise RuntimeError('failed guard reservation admitted storage: ' + text[-5000:])
                        break
                    if AUTO_CHECK_PAYLOAD_ID.decode().strip() in text:
                        selected = True
                        stop.set()
                    pending_expected=(0 if a.guard_pending_fail_at in (1,2) else 1 if a.guard_pending_fail_at else 2)
                    ext2_checker_failed=a.guard_ext2_bad_checker or a.guard_ext2_missing_checker
                    complete_expected=(0 if a.guard_complete_fail_at in (1,2) else 1 if a.guard_complete_fail_at else 2)
                    expected = (f'GUARD_COMPLETE_TEST_RESULT outcome={complete_expected}' if a.guard_complete_test else
                                'GUARD_EXT2_INCOMPLETE preen' if a.guard_ext2_test and ext2_checker_failed else
                                'GUARD_EXT2_VERIFIED_PENDING' if a.guard_ext2_test and not a.guard_pending_fail_at else
                                (f'GUARD_PENDING_TEST_RESULT outcome={pending_expected}' if a.guard_records=='valid' else
                                 'GUARD_PENDING_TEST_INCOMPLETE records=0' if a.guard_records in ('mismatch','wrong-identity') else
                                 'GUARD_PENDING_TEST_INCOMPLETE records=-2' if a.guard_records=='missing' else
                                 'GUARD_PENDING_TEST_INCOMPLETE records=-22') if a.guard_pending_test else
                                ('GUARD_RECORDS_MATCH' if a.guard_records=='valid' else
                                 'GUARD_RECORDS_INCOMPLETE mismatch' if a.guard_records in ('mismatch','wrong-identity') else
                                 'GUARD_RECORDS_INCOMPLETE read=-2' if a.guard_records=='missing' else
                                 'GUARD_RECORDS_INCOMPLETE read=-22') if a.guard_records else
                                'GUARD_CHECK_INCOMPLETE load=-8' if a.guard_bad_checker else
                                ('GUARD_CHECK_RESULT exit=1' if a.fat_crosslink else 'GUARD_CHECK_RESULT exit=0') if a.guard_real_checker else
                                'GUARD_OPEN_PROBE_PASS' if a.guard_open_probe else
                                ('FAT_CHECK_RESULT exit=1' if a.fat_crosslink else 'FAT_CHECK_RESULT exit=0') if a.fat_checker else
                                'RECOVERY_READONLY_CHECK_INCOMPLETE: e2fsck exit=8;' if a.prepared_pair else
                                'RECOVERY_READONLY_CHECK_INCOMPLETE: e2fsck exit=127;' if a.missing_checker else
                                'RECOVERY_READONLY_CHECK_INCOMPLETE: e2fsck exit=4;' if a.dirty else
                                'RECOVERY_READONLY_CHECK_CLEAN:')
                    complete_lines = [line.rstrip('\r') for line in text.split('\n')[:-1]]
                    if a.guard_ext2_test and any(line.startswith('GUARD_EXT2_INCOMPLETE ') and line!=expected for line in complete_lines):
                        raise RuntimeError('guarded ext2 repair failed: '+text[-4000:])
                    checker_expected='GUARD_CHECK_RESULT exit=0' if a.guard_records else expected
                    if a.guard_real_checker and any(
                            line.startswith(('GUARD_CHECK_RESULT ', 'GUARD_CHECK_INCOMPLETE ')) and line != checker_expected
                            for line in complete_lines):
                        raise RuntimeError('unexpected guard checker result: ' + text[-3000:])
                    if a.guard_open_probe and 'GUARD_OPEN_PROBE_FAILED' in text:
                        raise RuntimeError('guest guard open probe failed: ' + text[-3000:])
                    if a.fat_checker and not a.guard_real_checker and any(
                            line.startswith(('FAT_CHECK_FORK_FAILED', 'FAT_CHECK_EXEC_FAILED',
                                             'FAT_CHECK_WAIT_FAILED', 'FAT_CHECK_INTERRUPTED')) or
                            (line.startswith('FAT_CHECK_RESULT exit=') and line != expected)
                            for line in complete_lines):
                        raise RuntimeError('guest FAT checker failed: ' + text[-3000:])
                    matched = expected in complete_lines if a.fat_checker else expected in text
                    if a.guard_auto_restart and matched:
                        if complete_expected!=2:
                            time.sleep(2)
                            if 'GUARD_COMPLETE_TEST_RESTART' in serial.read_text(errors='replace'):
                                raise RuntimeError('guest restarted after incomplete completion')
                        else:
                            # Require the same QEMU process to return through EFI
                            # and reach normal boot after completion.
                            after=text.split(expected,1)[1]
                            if 'GUARD_COMPLETE_TEST_RESTART_RETURNED' in after:
                                raise RuntimeError('guest reset returned without restarting')
                            if 'GUARD_COMPLETE_TEST_RESTART' not in after or 'BOOT: complete' not in after:
                                time.sleep(.2);continue
                            if after.count('GUARD_COMPLETE_TEST_RESTART')!=1 or 'SSD_GUARD_RESERVATION_READY' in after:
                                raise RuntimeError('guest re-entered recovery after completion')
                    if selected and matched:
                        if a.guard_ext2_test and not a.guard_pending_fail_at:
                            preen=('GUARD_EXT2_PREEN exit=-8' if a.guard_ext2_bad_checker else
                                   'GUARD_EXT2_PREEN exit=-2' if a.guard_ext2_missing_checker else
                                   'GUARD_EXT2_PREEN exit=1' if a.dirty else 'GUARD_EXT2_PREEN exit=0')
                            required=['GUARD_CHECK_RESULT exit=0','GUARD_PENDING_TEST_RESULT outcome=2',preen,expected]
                            if a.guard_complete_test:required.insert(-1,'GUARD_EXT2_VERIFIED_PENDING')
                            if not all(line in complete_lines for line in required):
                                raise RuntimeError('guarded repair milestones missing')
                            indices=[complete_lines.index(line) for line in required]
                            if indices!=sorted(indices):raise RuntimeError('guarded repair milestones out of order')
                        if a.guard_records and ('GUARD_CHECK_RESULT exit=0' not in complete_lines or
                                                complete_lines.index('GUARD_CHECK_RESULT exit=0')>=complete_lines.index(expected)):
                            raise RuntimeError('guard record result did not follow a successful checker')
                        if a.guard_bad_checker and 'GUARD_CHECK_RESULT' in text:
                            raise RuntimeError('bad checker unexpectedly produced a result')
                        if a.guard_reservation:
                            reservation = 'SSD_GUARD_RESERVATION_READY' if a.guard_reservation == 'valid' else 'SSD_GUARD_RESERVATION_FAILED'
                            if reservation not in text or 'ahci system: verified GPT entry' in text or 'writable desktop: ready' in text:
                                raise RuntimeError('guard reservation or maintenance-only boot failed: ' + text[-5000:])
                        if a.prepared_pair and ('Pass 1' in text or
                                                'No such device while trying to open /dev/brook-install-disk2' not in text):
                            raise RuntimeError('physical recovery pair did not reject the wrong disk as expected')
                        if a.fat_crosslink and not all(s in text.lower() for s in ('/qa0.bin', '/qa1.bin', 'share clusters')):
                            raise RuntimeError('guest FAT checker did not report the crosslink')
                        if not (a.missing_checker or a.prepared_pair or a.fat_checker or a.guard_open_probe) and 'Pass 5' not in text:
                            raise RuntimeError('check did not report all filesystem passes')
                        if a.frame_output:
                            frame = a.frame_output.absolute()
                            if frame.exists() or not frame.parent.is_dir() or any(c.isspace() for c in str(frame)):
                                raise RuntimeError('frame output must be a new path without whitespace')
                            # Allow the compositor to show the final console
                            # line and its transient startup popup to expire.
                            time.sleep(7)
                            with socket.socket(socket.AF_UNIX) as connection:
                                connection.settimeout(3); connection.connect(str(monitor))
                                connection.sendall(('screendump ' + str(frame) + '\n').encode())
                            end = time.monotonic() + 5
                            while not frame.exists() and time.monotonic() < end:
                                time.sleep(.1)
                            if not frame.exists() or not frame.stat().st_size:
                                raise RuntimeError('QEMU did not write a frame')
                        break
                    if any((not a.prepared_pair and line.startswith('RECOVERY_READONLY_CHECK_INCOMPLETE:') and
                            expected not in line) or
                           (line.startswith('RECOVERY_READONLY_CHECK_EXEC_FAILED') and
                            not a.missing_checker) for line in complete_lines):
                        raise RuntimeError('local checker failed: ' + text[-3000:])
                    time.sleep(.2)
                else:
                    raise TimeoutError('local read-only check did not complete: ' + text[-3000:])
            finally:
                stop.set(); thread.join(timeout=3)
                process.terminate(); process.wait(timeout=10)
                if a.serial_output and serial.exists():
                    with a.serial_output.open('xb') as output:
                        output.write(serial.read_bytes())
        if a.guard_reservation in ('wrong-guid', 'wrong-fs-uuid') or a.guard_pending_no_marker or a.guard_pending_wrong_serial:
            final = serial.read_text(errors='replace')
            if (final.count('SSD_GUARD_RESERVATION_FAILED') != 1 or
                    'SSD guard reservation failed; no recovery access permitted' not in final or
                    'System halted.' not in final or
                    any(marker in final for marker in ('GUARD_OPEN_PROBE_', 'RECOVERY_READONLY_CHECK_',
                                                       'BOOT: 65% Modules', 'SSD_GUARD_RESERVATION_READY',
                                                       'ahci system: verified GPT entry', 'writable desktop: ready'))):
                raise RuntimeError('guard failure did not halt before later boot stages: ' + final[-5000:])
        if baseline:
            # Ensure only the fixed guard data and their directory entries
            # changed; GPT, data partition, FAT tables and boot code are not
            # allowed to change. This is a logical-write test, not a power cut.
            offset=2048*512;length=131072*512
            with baseline.open('rb') as source:
                source.seek(offset);esp=source.read(length)
            if len(esp)!=length:raise RuntimeError('short baseline ESP')
            reserved=struct.unpack_from('<H',esp,14)[0];nfats=esp[16]
            fat=struct.unpack_from('<H',esp,22)[0] or struct.unpack_from('<I',esp,36)[0]
            root=struct.unpack_from('<H',esp,17)[0];csize=esp[13]
            data_start=reserved+nfats*fat+(root+15)//16
            if nfats!=2 or not fat or not csize:raise RuntimeError('invalid baseline FAT geometry')
            allowed=[]
            expected_pending=bytearray(record_data)
            struct.pack_into('<QI',expected_pending,8,2,2)
            struct.pack_into('<I',expected_pending,124,zlib.crc32(expected_pending[:124]))
            if not a.guard_pending_fail_at:
                expected_records=[expected_pending,expected_pending]
            elif a.guard_pending_fail_at<=4:
                expected_records=[record_data,record_data]
            elif a.guard_pending_fail_at<=7:
                expected_records=[expected_pending,record_data]
            else:
                expected_records=[expected_pending,expected_pending]
            if a.guard_complete_test:
                expected_clean=bytearray(expected_pending)
                struct.pack_into('<I',expected_clean,16,1)
                struct.pack_into('<I',expected_clean,124,zlib.crc32(expected_clean[:124]))
                if not a.guard_complete_fail_at or a.guard_complete_fail_at>=8:
                    expected_records=[expected_clean,expected_clean]
                elif a.guard_complete_fail_at>=5:
                    expected_records=[expected_clean,expected_pending]
            attribute_offsets=[]
            actual_records=[]
            for index in range(2):
                name=(f'GUARD{index}  BIN').encode();positions=[];at=0
                while True:
                    at=esp.find(name,at)
                    if at<0:break
                    if at%32==0 and not (esp[at+11]&0x18):positions.append(at)
                    at+=1
                if len(positions)!=1:raise RuntimeError('guard directory entry not unique')
                at=positions[0];cluster=struct.unpack_from('<H',esp,at+26)[0] | (struct.unpack_from('<H',esp,at+20)[0]<<16)
                if cluster<2 or struct.unpack_from('<I',esp,at+28)[0]!=128:raise RuntimeError('invalid guard directory entry')
                guard_offset=(data_start+(cluster-2)*csize)*512
                # FatFs may update the archive bit, access date and modified
                # timestamp. Names, cluster pointers and sizes must not change.
                allowed += [(offset+at+11,offset+at+12),(offset+at+18,offset+at+20),
                            (offset+at+22,offset+at+26),(offset+guard_offset,offset+guard_offset+128)]
                attribute_offsets.append(offset+at+11)
                actual=subprocess.check_output(['mtype','-i',media,f'::/RECOVERY/GUARD{index}.BIN'])
                if actual!=expected_records[index]:
                    raise RuntimeError('pending record readback failed')
                actual_records.append(actual)
            if not a.guard_pending_fail_at and not a.guard_complete_fail_at and actual_records[0]!=actual_records[1]:
                raise RuntimeError('pending records differ')
            changes=0;pos=0
            if a.guard_ext2_test and not (a.guard_pending_fail_at or a.guard_ext2_bad_checker or a.guard_ext2_missing_checker):
                allowed.append((parts[1]['start']*512,(parts[1]['start']+parts[1]['size'])*512))
            with baseline.open('rb') as old,disk.open('rb') as new:
                while True:
                    left=old.read(1024*1024);right=new.read(1024*1024)
                    if len(left)!=len(right):raise RuntimeError('disk size changed')
                    if not left:break
                    if left!=right:
                        for i,(a_byte,b_byte) in enumerate(zip(left,right)):
                            if a_byte!=b_byte:
                                absolute=pos+i;changes+=1
                                if not any(start<=absolute<end for start,end in allowed):
                                    raise RuntimeError(f'unexpected disk write at byte {absolute}')
                                if absolute in attribute_offsets and ((a_byte^b_byte)&~0x20):
                                    raise RuntimeError('guard attributes changed beyond the archive bit')
                    pos+=len(left)
            if not changes and not a.guard_pending_fail_at:raise RuntimeError('pending transaction changed no bytes')
            if a.guard_ext2_test and not (a.guard_pending_fail_at or a.guard_ext2_bad_checker or a.guard_ext2_missing_checker):
                partition=work/'verify-data.img'
                subprocess.run(['dd',f'if={disk}',f'of={partition}','bs=512',
                                f'skip={parts[1]["start"]}',f'count={parts[1]["size"]}','status=none'],check=True)
                checked=subprocess.run(['e2fsck','-f','-n',str(partition)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                if checked.returncode:raise RuntimeError('independent host e2fsck failed: '+checked.stdout.decode(errors='replace')[-3000:])
                content=subprocess.check_output(['debugfs','-R','cat /brook-recovery-known',str(partition)],stderr=subprocess.DEVNULL)
                if content!=b'Brook guarded recovery fixture\n':raise RuntimeError('known file changed during recovery')
                with partition.open('rb') as stream:
                    stream.seek(1024+104)
                    if stream.read(16)!=filesystem_uuid:raise RuntimeError('filesystem identity changed during recovery')
            if a.guard_ext2_test and a.guard_pending_fail_at:
                final=serial.read_text(errors='replace')
                if 'GUARD_EXT2_PREEN' in final:raise RuntimeError('preen started after pending failure')
            if a.guarded_efi:
                guard_boot=work/'pending-boot';guard_boot.mkdir();pending_hash=sha(disk)
                clean=(expected_records[0]==expected_records[1] and struct.unpack_from('<I',expected_records[0],16)[0]==1)
                marker=('RECOVERY_NORMAL_DATA_READ_PASS' if clean and a.guard_desktop_kernel_build else
                        'BOOT: complete' if clean else 'Brook SSD recovery payload verified')
                boot_menu_fixture(disk,a.firmware.resolve(strict=True),guard_boot,False,marker,90,True,
                                  'Recovery required; normal boot blocked.' if clean else None,sata_snapshot=True,
                                  sata_serial='BROOK-RECOVERY-QA' if a.guard_desktop_kernel_build else None,
                                  failure_marker='recovery mount open' if a.guard_desktop_kernel_build and clean else None)
                guard_log=(guard_boot/'serial.log').read_text(errors='replace')
                if clean and a.guard_desktop_kernel_build:
                    direct=guard_log.find('ahci direct: ready ')
                    probe=guard_log.find('RECOVERY_NORMAL_DATA_READ_PASS')
                    if direct<0 or probe<=direct:
                        raise RuntimeError('normal boot did not admit and read the repaired data partition')
                if not clean and 'Recovery required; normal boot blocked.' not in guard_log:
                    raise RuntimeError('guarded EFI did not block normal boot for pending records')
                if sha(disk)!=pending_hash:raise RuntimeError('guarded reboot changed the source disk')
        elif sha(disk) != before:
            raise RuntimeError('read-only check changed the disposable disk')
        if poison and sha(poison) != poison_before:
            raise RuntimeError('guard boot changed the overmount fixture')
        print('RECOVERY_GUEST_GUARD_EXT2_REJECT_PASS' if a.guard_ext2_test and (a.guard_pending_fail_at or a.guard_ext2_bad_checker or a.guard_ext2_missing_checker) else
              'RECOVERY_GUEST_GUARD_EXT2_PASS' if a.guard_ext2_test else
              'RECOVERY_GUEST_GUARD_REJECT_PASS' if a.guard_reservation in ('wrong-guid', 'wrong-fs-uuid') or a.guard_bad_checker or a.guard_pending_no_marker or a.guard_pending_wrong_serial else
              'RECOVERY_GUEST_GUARD_PENDING_PASS' if a.guard_pending_test else
              'RECOVERY_GUEST_GUARD_CHECK_PASS' if a.guard_real_checker else
              'RECOVERY_GUEST_GUARD_OPEN_PASS' if a.guard_open_probe else
              'RECOVERY_GUEST_FAT_CROSSLINK_PASS' if a.fat_crosslink else
              'RECOVERY_GUEST_FAT_CHECK_PASS' if a.fat_checker else
              'RECOVERY_PHYSICAL_PAIR_REJECTS_QEMU_DISK_PASS' if a.prepared_pair else
              'RECOVERY_LOCAL_NO_NETWORK_MISSING_CHECKER_PASS' if a.missing_checker else
              'RECOVERY_LOCAL_NO_NETWORK_DIRTY_READONLY_PASS' if a.dirty else
              'RECOVERY_LOCAL_NO_NETWORK_READONLY_PASS')


if __name__ == '__main__':
    main()

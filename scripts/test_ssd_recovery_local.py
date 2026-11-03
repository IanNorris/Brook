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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_update_protocol import record
from hardware_recovery_fsck import validate_checker
from prepare_hardware_kernel_pair import module_version
from prepare_ssd_recovery_pair import AUTO_CHECK_INIT, AUTO_CHECK_PAYLOAD_ID


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
    p.add_argument('--guard-reservation', choices=('valid', 'wrong-guid'),
                   help='Configure the disposable guard-only kernel reservation fixture')
    p.add_argument('--guard-poison-boot', action='store_true',
                   help='Attach a disposable virtio disk requesting an overmount of /boot')
    a = p.parse_args()
    if a.dirty and a.missing_checker:
        p.error('choose one failure scenario')
    if a.prepared_pair and (a.dirty or a.missing_checker or a.kernel_build or a.fat_checker):
        p.error('prepared pair is an independent identity rejection scenario')
    if a.fat_checker and (a.dirty or a.missing_checker):
        p.error('FAT checker uses a clean fixture')
    if a.fat_crosslink and not a.fat_checker:
        p.error('--fat-crosslink requires --fat-checker')
    if a.guard_open_probe and (a.dirty or a.missing_checker or a.fat_checker or a.prepared_pair):
        p.error('guard open probe is an independent scenario')
    if a.guard_reservation and (not a.guard_open_probe or not a.kernel_build):
        p.error('guard reservation requires a guard-only kernel and guard-open probe')
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
        if a.dirty:
            partition = work / 'partition.img'
            subprocess.run(['dd', f'if={disk}', f'of={partition}', 'bs=512',
                            'skip=133120', 'count=126976', 'status=none'], check=True)
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
            disk_guid = uuid.UUID(table['id']).bytes_le.hex()
            esp_guid = uuid.UUID(parts[0]['uuid']).bytes_le.hex()
            if a.guard_reservation == 'wrong-guid':
                esp_guid = ('0' if esp_guid[0] != '0' else '1') + esp_guid[1:]
            data_guid = uuid.UUID(parts[1]['uuid']).bytes_le.hex()
            records = {
                'ahci-write-target': 'BRKAHCI1\n32902,10530,0,31,2,0,262144,512,512\nQEMU HARDDISK\nBROOK-RECOVERY-QA\n',
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
        init = work / 'INIT.RC'; init.write_bytes(AUTO_CHECK_INIT)
        ident = work / 'hardware-payload-id'; ident.write_bytes(AUTO_CHECK_PAYLOAD_ID)
        if not a.prepared_pair:
            for source, target in ((init, '::/BOOT/INIT.RC'),
                                   (ident, '::/ETC/hardware-payload-id'),
                                   (a.supervisor, '::/BOOT/RECCHECK.ELF')):
                subprocess.run(['mcopy', '-o', '-i', str(initrd), str(source), target], check=True)
            if a.fat_checker:
                subprocess.run(['mcopy', '-o', '-i', str(initrd), str(a.fat_checker), '::/BOOT/FATFSCK.ELF'], check=True)
                copied=subprocess.check_output(['mtype','-i',str(initrd),'::/BOOT/FATFSCK.ELF'])
                if len(copied)!=fat_info['bytes'] or hashlib.sha256(copied).hexdigest()!=fat_info['sha256']:
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
        before = sha(disk)
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
                   '-device', 'ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=BROOK-RECOVERY-QA',
                   '-display', 'none', '-serial', f'file:{serial}',
                   '-monitor', f'unix:{monitor},server=on,wait=off', '-no-reboot']
        if poison:
            command += ['-drive', f'if=none,id=poison,format=raw,file={poison}',
                        '-device', 'virtio-blk-pci,drive=poison']
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
                        raise RuntimeError('QEMU exited before local check completed')
                    text = serial.read_text(errors='replace') if serial.exists() else ''
                    if (a.guard_reservation == 'wrong-guid' and
                            'SSD guard reservation failed; no recovery access permitted' in text and
                            'System halted.' in text):
                        if 'SSD_GUARD_RESERVATION_READY' in text or 'ahci system: verified GPT entry' in text:
                            raise RuntimeError('failed guard reservation admitted storage: ' + text[-5000:])
                        break
                    if AUTO_CHECK_PAYLOAD_ID.decode().strip() in text:
                        selected = True
                        stop.set()
                    expected = ('GUARD_OPEN_PROBE_PASS' if a.guard_open_probe else
                                ('FAT_CHECK_RESULT exit=1' if a.fat_crosslink else 'FAT_CHECK_RESULT exit=0') if a.fat_checker else
                                'RECOVERY_READONLY_CHECK_INCOMPLETE: e2fsck exit=8;' if a.prepared_pair else
                                'RECOVERY_READONLY_CHECK_INCOMPLETE: e2fsck exit=127;' if a.missing_checker else
                                'RECOVERY_READONLY_CHECK_INCOMPLETE: e2fsck exit=4;' if a.dirty else
                                'RECOVERY_READONLY_CHECK_CLEAN:')
                    complete_lines = [line.rstrip('\r') for line in text.split('\n')[:-1]]
                    if a.guard_open_probe and 'GUARD_OPEN_PROBE_FAILED' in text:
                        raise RuntimeError('guest guard open probe failed: ' + text[-3000:])
                    if a.fat_checker and any(
                            line.startswith(('FAT_CHECK_FORK_FAILED', 'FAT_CHECK_EXEC_FAILED',
                                             'FAT_CHECK_WAIT_FAILED', 'FAT_CHECK_INTERRUPTED')) or
                            (line.startswith('FAT_CHECK_RESULT exit=') and line != expected)
                            for line in complete_lines):
                        raise RuntimeError('guest FAT checker failed: ' + text[-3000:])
                    matched = expected in complete_lines if a.fat_checker else expected in text
                    if selected and matched:
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
        if a.guard_reservation == 'wrong-guid':
            final = serial.read_text(errors='replace')
            if (final.count('SSD_GUARD_RESERVATION_FAILED') != 1 or
                    'SSD guard reservation failed; no recovery access permitted' not in final or
                    'System halted.' not in final or
                    any(marker in final for marker in ('GUARD_OPEN_PROBE_', 'RECOVERY_READONLY_CHECK_',
                                                       'BOOT: 65% Modules', 'SSD_GUARD_RESERVATION_READY',
                                                       'ahci system: verified GPT entry', 'writable desktop: ready'))):
                raise RuntimeError('guard failure did not halt before later boot stages: ' + final[-5000:])
        if sha(disk) != before:
            raise RuntimeError('read-only check changed the disposable disk')
        if poison and sha(poison) != poison_before:
            raise RuntimeError('guard boot changed the overmount fixture')
        print('RECOVERY_GUEST_GUARD_REJECT_PASS' if a.guard_reservation == 'wrong-guid' else
              'RECOVERY_GUEST_GUARD_OPEN_PASS' if a.guard_open_probe else
              'RECOVERY_GUEST_FAT_CROSSLINK_PASS' if a.fat_crosslink else
              'RECOVERY_GUEST_FAT_CHECK_PASS' if a.fat_checker else
              'RECOVERY_PHYSICAL_PAIR_REJECTS_QEMU_DISK_PASS' if a.prepared_pair else
              'RECOVERY_LOCAL_NO_NETWORK_MISSING_CHECKER_PASS' if a.missing_checker else
              'RECOVERY_LOCAL_NO_NETWORK_DIRTY_READONLY_PASS' if a.dirty else
              'RECOVERY_LOCAL_NO_NETWORK_READONLY_PASS')


if __name__ == '__main__':
    main()

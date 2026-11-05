#!/usr/bin/env python3
"""Panic-capture a packet to a separate disposable QEMU SATA disk.

The kernel must be built with BROOK_AHCI_PANIC_TEST=ON. The source boot image
is copied and never opened writable. This uses TCG; it does not test the Dell.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import zlib

SECTOR = 512
BASE = 4096
SLOT_SECTORS = 129
REGION_END = (BASE + 2 * SLOT_SECTORS) * SECTOR
DISK_BYTES = 131072 * SECTOR


def verify_scratch(path, expected, slot=0, sequence=1):
    with path.open('rb') as stream:
        data = stream.read()
    if len(data) != DISK_BYTES:
        raise RuntimeError('scratch size changed')
    if any(data[:BASE * SECTOR]) or any(data[REGION_END:]):
        raise RuntimeError('write escaped the reserved test region')
    if expected is None:
        if any(data[BASE * SECTOR:REGION_END]):
            raise RuntimeError('rejected test changed the scratch region')
        return
    start = BASE + slot * SLOT_SECTORS
    header = bytearray(data[start * SECTOR:(start + 1) * SECTOR])
    if header[:8] != b'BRKPAN01':
        raise RuntimeError('journal header missing')
    version, length, seq, checksum, header_crc = struct.unpack_from('<IIQII', header, 8)
    struct.pack_into('<I', header, 28, 0)
    if version != 1 or seq != sequence or zlib.crc32(header) != header_crc:
        raise RuntimeError('invalid journal header or checksum')
    packet = data[(start + 1) * SECTOR:(start + 1) * SECTOR + length]
    if packet != expected or zlib.crc32(packet) != checksum:
        raise RuntimeError('disk packet differs from serial panic packet')


def run_case(work, image, kernel, firmware, marker, serial_number, timeout, repeat=False,
             command='panic disk_capture_test', corrupt=False, busy=False):
    boot = work / 'boot.raw'
    shutil.copyfile(image, boot)
    esp = str(boot) + '@@1048576'
    initrd = work / 'initrd.img'
    subprocess.run(['mcopy', '-i', esp, '::/INITRD.IMG', str(initrd)], check=True)
    records = {'ahci-observe': '1\n', 'ahci-read-only': '1\n',
               'ahci-write-target': 'BRKAHCI1\n32902,10530,0,31,2,0,131072,512,512\n'
                                    'QEMU HARDDISK\nBROOK-PANIC-QA\n'}
    for name, content in records.items():
        item = work / name
        item.write_text(content)
        subprocess.run(['mcopy', '-o', '-i', str(initrd), str(item), '::/ETC/' + name], check=True)
    init = work / 'INIT.RC'
    init.write_text(command + '\n')
    subprocess.run(['mcopy', '-o', '-i', str(initrd), str(init), '::/BOOT/INIT.RC'], check=True)
    subprocess.run(['mcopy', '-o', '-i', esp, str(initrd), '::/INITRD.IMG'], check=True)
    subprocess.run(['mcopy', '-o', '-i', esp, str(kernel), '::/KERNEL/BROOK.ELF'], check=True)
    scratch = work / 'scratch.raw'
    with scratch.open('wb') as stream:
        stream.truncate(DISK_BYTES)
        if corrupt:
            stream.seek(BASE * SECTOR)
            stream.write(b'\x01')
    initial_hash = hashlib.sha256(scratch.read_bytes()).digest()
    variables = work / 'vars.fd'
    shutil.copyfile(firmware / 'OVMF_VARS.fd', variables)
    serial = work / 'serial.log'
    args = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-cpu', 'max',
            '-smp', '2', '-m', '1024', '-nodefaults',
            '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
            '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
            '-device', 'qemu-xhci,id=usbc', '-drive', f'if=none,id=boot,format=raw,file={boot}',
            '-device', 'usb-storage,bus=usbc.0,port=1,drive=boot,bootindex=1',
            '-drive', f'if=none,id=scratch,format=raw,file={scratch}',
            '-device', f'ide-hd,drive=scratch,bus=ide.0,serial={serial_number}',
            '-display', 'none', '-serial', f'file:{serial}', '-no-reboot',
            '-fw_cfg', 'name=opt/brook/power,string=q35-reset',
            '-fw_cfg', 'name=opt/brook/panic-test-reset,string=test-1s']
    if marker:
        args += ['-fw_cfg', 'name=opt/brook/panic-test-disk,string=journal-1']
    if busy:
        args += ['-fw_cfg', 'name=opt/brook/panic-test-busy,string=1']
    capture = marker and serial_number == 'BROOK-PANIC-QA' and not corrupt and not busy
    first_slot = None
    for boot_number in range(2 if repeat else 1):
        if serial.exists():
            serial.unlink()
        with (work / f'qemu-{boot_number}.log').open('wb') as errors:
            proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                try:
                    status = proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    output = serial.read_text(errors='replace') if serial.exists() else ''
                    raise RuntimeError('timed out: ' + output[-4000:])
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
        output = serial.read_text(errors='replace') if serial.exists() else ''
        if status != 0 or 'PANIC_TEST_RESET_WAIT' not in output:
            raise RuntimeError(f'panic/reset failed, exit={status}: {output[-4000:]}')
        if not capture:
            if busy:
                if 'PANIC_TEST_DISK_BUSY_ARMED' not in output or 'PANIC_TEST_DISK_BUSY\n' not in output:
                    raise RuntimeError('busy ownership was not observed: ' + output[-4000:])
            elif 'PANIC_TEST_DISK_READY' in output:
                raise RuntimeError('rejected disk was unexpectedly admitted')
            if 'PANIC_TEST_DISK_CAPTURE_OK' in output:
                raise RuntimeError('unexpected capture on rejected or busy disk')
            if hashlib.sha256(scratch.read_bytes()).digest() != initial_hash:
                raise RuntimeError('rejected disk was modified')
            return
        if 'PANIC_TEST_DISK_READY' not in output or 'PANIC_TEST_DISK_CAPTURE_OK' not in output:
            raise RuntimeError('disk capture not confirmed: ' + output[-4000:])
        line = next((x for x in output.splitlines() if x.startswith('PANIC_HEX:')), None)
        if not line:
            raise RuntimeError('structured serial packet missing')
        verify_scratch(scratch, bytes.fromhex(line[len('PANIC_HEX:'):]), boot_number, boot_number + 1)
        with scratch.open('rb') as stream:
            stream.seek(BASE * SECTOR)
            now = stream.read(SLOT_SECTORS * SECTOR)
        if boot_number == 0:
            first_slot = now
        elif now != first_slot:
            raise RuntimeError('second capture modified the previous slot')



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True, help='128 MiB GPT boot fixture with ESP at sector 2048')
    parser.add_argument('--kernel', type=Path, required=True, help='BROOK_AHCI_PANIC_TEST kernel')
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--case', choices=['capture', 'exception', 'unarmed', 'wrong-serial', 'corrupt-journal', 'busy'])
    args = parser.parse_args()
    image, kernel, firmware = (p.resolve(strict=True) for p in (args.image, args.kernel, args.firmware))
    with tempfile.TemporaryDirectory(prefix='brook-panic-ahci-') as temp:
        cases = [('capture', True, 'BROOK-PANIC-QA', 'panic disk_capture_test', False),
                 ('exception', True, 'BROOK-PANIC-QA', 'crashtest ud', False),
                 ('unarmed', False, 'BROOK-PANIC-QA', 'panic unarmed', False),
                 ('wrong-serial', True, 'BROOK-WRONG-QA', 'panic wrong_serial', False),
                 ('corrupt-journal', True, 'BROOK-PANIC-QA', 'panic corrupt_journal', True),
                 ('busy', True, 'BROOK-PANIC-QA', 'panic busy', False)]
        for name, marker, serial, command, corrupt in cases:
            if args.case and args.case != name:
                continue
            work = Path(temp) / name
            work.mkdir()
            run_case(work, image, kernel, firmware, marker, serial, args.timeout,
                     repeat=name == 'capture', command=command, corrupt=corrupt, busy=name == 'busy')
            print(f'{name}: PASS', flush=True)


if __name__ == '__main__':
    main()

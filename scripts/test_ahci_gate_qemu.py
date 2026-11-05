#!/usr/bin/env python3
"""Verify ordinary AHCI I/O with the panic gate on a disposable SATA copy."""
import argparse
import json
import shutil
import subprocess
import tempfile
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--firmware', required=True, type=Path)
    parser.add_argument('--compiler', default='x86_64-unknown-linux-musl-gcc')
    parser.add_argument('--timeout', default=75, type=int)
    args = parser.parse_args()
    source = args.image.resolve(strict=True)
    kernel = args.kernel.resolve(strict=True)
    firmware = args.firmware.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='brook-ahci-gate-') as temp:
        d = Path(temp)
        disk = d / 'disk.raw'
        shutil.copyfile(source, disk)
        table = json.loads(subprocess.check_output(['sfdisk', '--json', str(disk)]))['partitiontable']
        if table['label'] != 'gpt' or disk.stat().st_size != 262144 * 512:
            raise RuntimeError('expected the 128 MiB GPT test fixture')
        part = table['partitions'][0]
        esp = f'{disk}@@{part["start"] * 512}'
        initrd = d / 'initrd.img'
        subprocess.run(['mcopy', '-i', esp, '::/INITRD.IMG', str(initrd)], check=True)
        for directory in ('MEDIA', 'MEDIA/SYSTEM'):
            subprocess.run(['mmd', '-i', str(initrd), '::/' + directory], check=True)
        records = {
            'ahci-observe': '1\n',
            'ahci-read-only': '1\n',
            'ahci-write-target': 'BRKAHCI1\n32902,10530,0,31,2,0,262144,512,512\nQEMU HARDDISK\nBROOK-GATE-TEST\n',
            'ahci-boot-volume': f'BRKESP1\n{uuid.UUID(table["id"]).bytes_le.hex()}\n'
                                f'{uuid.UUID(part["uuid"]).bytes_le.hex()}\n'
                                f'{part["start"]},{part["size"]}\n',
        }
        for name, content in records.items():
            item = d / name
            item.write_text(content)
            subprocess.run(['mcopy', '-o', '-i', str(initrd), str(item), '::/ETC/' + name], check=True)
        init = d / 'INIT.RC'
        init.write_text('set wm\nset vfb none\nsched eevdf\nrun --console --uid 0 /boot/GATEQA.ELF\n')
        subprocess.run(['mcopy', '-o', '-i', str(initrd), str(init), '::/BOOT/INIT.RC'], check=True)
        binary = d / 'gate.elf'
        subprocess.run([args.compiler, '-static', '-O2', str(ROOT / 'scripts/workloads/ahci_gate_smoke.c'),
                        '-o', str(binary)], check=True)
        subprocess.run(['mcopy', '-o', '-i', str(initrd), str(binary), '::/BOOT/GATEQA.ELF'], check=True)
        subprocess.run(['mcopy', '-o', '-i', esp, str(initrd), '::/INITRD.IMG'], check=True)
        subprocess.run(['mcopy', '-o', '-i', esp, str(kernel), '::/KERNEL/BROOK.ELF'], check=True)
        variables = d / 'vars.fd'
        shutil.copyfile(firmware / 'OVMF_VARS.fd', variables)
        serial = d / 'serial.log'
        qemu = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-cpu', 'max',
                '-smp', '2', '-m', '1024', '-nodefaults',
                '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
                '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
                '-drive', f'if=none,id=stick,format=raw,file={disk}',
                '-device', 'ide-hd,drive=stick,bus=ide.0,bootindex=1,serial=BROOK-GATE-TEST',
                '-display', 'none', '-serial', f'file:{serial}', '-no-reboot']
        with (d / 'qemu.log').open('wb') as errors:
            process = subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    output = serial.read_text(errors='replace') if serial.exists() else ''
                    if 'AHCI_GATE_IO_SMOKE_OK' in output:
                        if 'ahci system: verified GPT entry 1 mounted' not in output:
                            raise RuntimeError('the intended AHCI volume was not mounted')
                        break
                    if process.poll() is not None:
                        raise RuntimeError('QEMU exited: ' + output[-3000:])
                    time.sleep(0.2)
                else:
                    raise RuntimeError('timed out: ' + output[-3000:])
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
        result = subprocess.check_output(['mtype', '-i', esp, '::/GATEQA.TXT'])
        if result != b'brook-ahci-gate-smoke\n':
            raise RuntimeError('independent FAT readback mismatch')
        print('AHCI gate QEMU read/write/sync and independent readback: PASS')


if __name__ == '__main__':
    main()

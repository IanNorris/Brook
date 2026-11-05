#!/usr/bin/env python3
"""Check idle flush using the direct SATA desktop path on disposable disks.

The application tree is synthetic. QEMU process kill is not physical power
loss, and this test does not establish SSD controller-cache durability.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
DIRS = ('store', 'profile', 'profile/bin', 'profile/lib', 'profile/share',
        'profile/share/applications', 'home', 'home/brook', 'var', 'var/cache',
        'var/cache/narinfo', 'share', 'share/applications', 'index', 'bin')
FILES = ('profile/manifest.tsv', 'index/packages.idx', 'share/applications/applications.idx',
         'bin/nix', 'bin/nix-fetch', 'bin/nar-unpack', 'profile/bin/brook-firefox',
         'profile/bin/brook-chromium', 'profile/bin/brook-gallery')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--compiler', default='x86_64-unknown-linux-musl-gcc')
    parser.add_argument('--timeout', type=int, default=100)
    args = parser.parse_args()
    source, kernel, firmware = (p.resolve(strict=True) for p in (args.image, args.kernel, args.firmware))
    with tempfile.TemporaryDirectory(prefix='brook-ext2-idle-sata-') as temp:
        d = Path(temp)
        boot, sata = d / 'boot.raw', d / 'sata.raw'
        shutil.copyfile(source, boot)
        shutil.copyfile(source, sata)
        table = json.loads(subprocess.check_output(['sfdisk', '--json', str(sata)]))['partitiontable']
        if table['label'] != 'gpt' or sata.stat().st_size != 262144 * 512 or len(table['partitions']) != 2:
            raise RuntimeError('expected the 128 MiB GPT fixture')
        esp_part, data_part = table['partitions']
        if esp_part['start'] != 2048:
            raise RuntimeError('unexpected EFI partition offset')
        disk_guid = uuid.UUID(table['id']).bytes_le.hex()
        with sata.open('rb') as stream:
            stream.seek(data_part['start'] * 512 + 1024 + 104)
            fs_uuid = stream.read(16)
        if len(fs_uuid) != 16 or not any(fs_uuid):
            raise RuntimeError('fixture filesystem UUID is absent')
        tree = d / 'tree'
        tree.mkdir()
        for name in DIRS:
            (tree / name).mkdir(parents=True, exist_ok=True)
        for name in FILES:
            file = tree / name
            file.write_text('synthetic fixture\n')
            file.chmod(0o755)
        part = d / 'data.img'
        with part.open('wb') as stream:
            stream.truncate(data_part['size'] * 512)
        subprocess.run(['mkfs.ext2', '-q', '-F', '-b', '4096', '-I', '128', '-N', '1024',
                        '-m', '0', '-O', 'none,filetype,sparse_super,large_file',
                        '-U', str(uuid.UUID(bytes=fs_uuid)), '-d', str(tree), str(part)], check=True)
        for name in DIRS:
            for field in ('uid', 'gid'):
                subprocess.run(['debugfs', '-w', '-R', f'set_inode_field /{name} {field} 1000', str(part)],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with sata.open('r+b') as output, part.open('rb') as input_file:
            output.seek(data_part['start'] * 512)
            shutil.copyfileobj(input_file, output, length=1024 * 1024)
        esp = str(boot) + '@@1048576'
        initrd = d / 'initrd.img'
        subprocess.run(['mcopy', '-i', esp, '::/INITRD.IMG', str(initrd)], check=True)
        for name in ('DATA', 'NIX', 'MEDIA', 'MEDIA/SYSTEM', 'MNT', 'MNT/PROBE', 'PROC'):
            subprocess.run(['mmd', '-i', str(initrd), '::/' + name], check=True)
        records = {
            'ahci-observe': '1\n', 'ahci-read-only': '1\n', 'volatile-desktop': '1\n',
            'ahci-write-target': 'BRKAHCI1\n32902,10530,0,31,2,0,262144,512,512\nQEMU HARDDISK\nBROOK-IDLE-TEST\n',
            'ahci-boot-volume': f'BRKESP1\n{disk_guid}\n{uuid.UUID(esp_part["uuid"]).bytes_le.hex()}\n'
                                f'{esp_part["start"]},{esp_part["size"]}\n',
            'ahci-data-volume': f'BRKDATA1\n{disk_guid}\n{uuid.UUID(data_part["uuid"]).bytes_le.hex()}\n'
                                f'{data_part["start"]},{data_part["size"]}\n{fs_uuid.hex()}\n',
        }
        for name, content in records.items():
            item = d / name
            item.write_text(content)
            subprocess.run(['mcopy', '-o', '-i', str(initrd), str(item), '::/ETC/' + name], check=True)
        init = d / 'INIT.RC'
        init.write_text('set wm\nset vfb none\nsched eevdf\nrun --console --uid 0 /boot/IDLEQA.ELF /data/home/IDLE.TXT\n')
        subprocess.run(['mcopy', '-o', '-i', str(initrd), str(init), '::/BOOT/INIT.RC'], check=True)
        binary = d / 'idle.elf'
        subprocess.run([args.compiler, '-static', '-O2', str(ROOT / 'scripts/workloads/ext2_idle_flush.c'),
                        '-o', str(binary)], check=True)
        subprocess.run(['mcopy', '-o', '-i', str(initrd), str(binary), '::/BOOT/IDLEQA.ELF'], check=True)
        subprocess.run(['mcopy', '-o', '-i', esp, str(initrd), '::/INITRD.IMG'], check=True)
        subprocess.run(['mcopy', '-o', '-i', esp, str(kernel), '::/KERNEL/BROOK.ELF'], check=True)
        variables = d / 'vars.fd'
        shutil.copyfile(firmware / 'OVMF_VARS.fd', variables)
        serial = d / 'serial.log'
        command = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-cpu', 'max',
                   '-smp', '2', '-m', '1024', '-nodefaults',
                   '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
                   '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
                   '-device', 'qemu-xhci,id=usbc', '-drive', f'if=none,id=boot,format=raw,file={boot}',
                   '-device', 'usb-storage,bus=usbc.0,port=1,drive=boot,bootindex=1',
                   '-drive', f'if=none,id=sata,format=raw,file={sata}',
                   '-device', 'ide-hd,drive=sata,bus=ide.0,serial=BROOK-IDLE-TEST',
                   '-display', 'none', '-serial', f'file:{serial}', '-no-reboot']
        with (d / 'qemu.log').open('wb') as errors:
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    output = serial.read_text(errors='replace') if serial.exists() else ''
                    if 'IDLE_FLUSH_OK' in output:
                        if 'ahci direct: ready' not in output:
                            raise RuntimeError('the intended direct data volume was not mounted')
                        print('\n'.join(line for line in output.splitlines() if 'IDLE_' in line), flush=True)
                        break
                    if any(x in output for x in ('IDLE_PROBE_FAILED', 'IDLE_NO_PENDING_WORK', 'IDLE_FLUSH_FAILED')):
                        raise RuntimeError(output[-4500:])
                    if process.poll() is not None:
                        raise RuntimeError('QEMU exited: ' + output[-4500:])
                    time.sleep(0.2)
                else:
                    raise RuntimeError('timed out: ' + output[-4500:])
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        with sata.open('rb') as input_file, part.open('wb') as output:
            input_file.seek(data_part['start'] * 512)
            remaining = data_part['size'] * 512
            while remaining:
                block = input_file.read(min(1024 * 1024, remaining))
                if not block:
                    raise RuntimeError('short partition read')
                output.write(block)
                remaining -= len(block)
        subprocess.run(['e2fsck', '-fn', str(part)], check=True, timeout=30)
        data = subprocess.check_output(['debugfs', '-R', 'cat /home/brook/IDLE.TXT', str(part)],
                                       stderr=subprocess.DEVNULL)
        if data != b'periodic ext2 durability witness\n':
            raise RuntimeError('offline file contents do not match')
        print('direct SATA idle flush and offline readback: PASS')


if __name__ == '__main__':
    main()

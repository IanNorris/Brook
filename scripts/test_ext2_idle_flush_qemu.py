#!/usr/bin/env python3
"""Verify idle ext2 flushing on a disposable virtio disk without guest fsync.

QEMU is killed after the guest reports a successful background flush. An
independent host e2fsck and debugfs read then inspect the test image. Process
termination is not a simulation of physical power loss or controller caches.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--firmware', required=True, type=Path)
    parser.add_argument('--compiler', default='x86_64-unknown-linux-musl-gcc')
    parser.add_argument('--timeout', type=int, default=90)
    args = parser.parse_args()
    source, kernel, firmware = (p.resolve(strict=True) for p in (args.image, args.kernel, args.firmware))
    with tempfile.TemporaryDirectory(prefix='brook-ext2-idle-') as temp:
        d = Path(temp)
        boot = d / 'boot.raw'
        shutil.copyfile(source, boot)
        esp = str(boot) + '@@1048576'
        initrd = d / 'initrd.img'
        subprocess.run(['mcopy', '-i', esp, '::/INITRD.IMG', str(initrd)], check=True)
        for name in ('DATA', 'MNT', 'MNT/PROBE', 'PROC'):
            subprocess.run(['mmd', '-i', str(initrd), '::/' + name], check=True)
        script = d / 'INIT.RC'
        script.write_text('set wm\nset vfb none\nsched eevdf\nrun --console --uid 0 /boot/IDLEQA.ELF\n')
        subprocess.run(['mcopy', '-o', '-i', str(initrd), str(script), '::/BOOT/INIT.RC'], check=True)
        binary = d / 'idle.elf'
        subprocess.run([args.compiler, '-static', '-O2', str(ROOT / 'scripts/workloads/ext2_idle_flush.c'),
                        '-o', str(binary)], check=True)
        subprocess.run(['mcopy', '-o', '-i', str(initrd), str(binary), '::/BOOT/IDLEQA.ELF'], check=True)
        subprocess.run(['mcopy', '-o', '-i', esp, str(initrd), '::/INITRD.IMG'], check=True)
        subprocess.run(['mcopy', '-o', '-i', esp, str(kernel), '::/KERNEL/BROOK.ELF'], check=True)
        tree = d / 'tree'
        tree.mkdir()
        (tree / 'BROOK.MNT').write_text('/data\n')
        disk = d / 'data.img'
        with disk.open('wb') as out:
            out.truncate(32 * 1024 * 1024)
        subprocess.run(['mkfs.ext2', '-q', '-F', '-b', '1024', '-I', '256', '-d', str(tree), str(disk)], check=True)
        variables = d / 'vars.fd'
        shutil.copyfile(firmware / 'OVMF_VARS.fd', variables)
        serial = d / 'serial.log'
        command = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-cpu', 'max',
                   '-smp', '2', '-m', '1024', '-nodefaults',
                   '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
                   '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
                   '-drive', f'if=none,id=boot,format=raw,file={boot}',
                   '-device', 'ide-hd,drive=boot,bus=ide.0,bootindex=1',
                   '-drive', f'if=none,id=data,format=raw,file={disk}',
                   '-device', 'virtio-blk-pci,drive=data', '-display', 'none',
                   '-serial', f'file:{serial}', '-no-reboot']
        with (d / 'qemu.log').open('wb') as errors:
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    output = serial.read_text(errors='replace') if serial.exists() else ''
                    if 'IDLE_FLUSH_OK' in output:
                        print('\n'.join(line for line in output.splitlines() if 'IDLE_' in line), flush=True)
                        break
                    if any(x in output for x in ('IDLE_PROBE_FAILED', 'IDLE_NO_PENDING_WORK', 'IDLE_FLUSH_FAILED')):
                        raise RuntimeError(output[-3500:])
                    if process.poll() is not None:
                        raise RuntimeError('QEMU exited: ' + output[-3500:])
                    time.sleep(0.2)
                else:
                    raise RuntimeError('timed out: ' + output[-3500:])
            finally:
                # Disposable test: intentionally omit guest shutdown/sync.
                if process.poll() is None:
                    process.kill()
                    process.wait()
        subprocess.run(['e2fsck', '-fn', str(disk)], check=True, timeout=30)
        data = subprocess.check_output(['debugfs', '-R', 'cat /IDLE.TXT', str(disk)],
                                       stderr=subprocess.DEVNULL)
        if data != b'periodic ext2 durability witness\n':
            raise RuntimeError('offline file contents do not match')
        print('offline ext2 check and readback: PASS')


if __name__ == '__main__':
    main()

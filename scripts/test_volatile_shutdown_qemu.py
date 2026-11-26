#!/usr/bin/env python3
"""Fill bounded /tmp and the RAM initrd in a disposable guest, then restart."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from test_input_preferences_qemu import wait_for

ROOT = Path(__file__).resolve().parents[1]
GUEST = r"""
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void check(int ok, const char *operation) {
    if (ok) return;
    perror(operation);
    exit(1);
}

static void fill(const char *path) {
    static char pages[65536];
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    check(fd >= 0, path);
    size_t bytes = 0;
    while (bytes < 256 * 1024 * 1024) {
        ssize_t n = write(fd, pages, sizeof(pages));
        if (n < 0) {
            check(errno == ENOSPC, "fill must report ENOSPC");
            break;
        }
        check(n > 0, "write returned no progress");
        bytes += (size_t)n;
    }
    check(bytes && bytes < 256 * 1024 * 1024, "quota reached");
    check(fsync(fd) == 0, "full filesystem fsync");
    check(close(fd) == 0, "close filled file");
    printf("VOLATILE_FULL_ENOSPC %s %zu\n", path, bytes);
}

int main(void) {
    struct stat root, tmp;
    check(stat("/", &root) == 0 && stat("/tmp", &tmp) == 0 && root.st_dev != tmp.st_dev,
          "/tmp separate from root");
    fill("/tmp/fill");
    fill("/VOLFULL.DAT");
    puts("VOLATILE_FULL_PASS");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    args = parser.parse_args()
    kvm = os.access('/dev/kvm', os.R_OK | os.W_OK)
    if kvm and subprocess.run(['pgrep', '-x', 'vmware-vmx']).returncode == 0:
        parser.error('Workstation is running')
    output = args.output.resolve()
    output.mkdir(parents=True)
    source = output / 'fill.c'
    source.write_text(GUEST)
    guest = output / 'FILL.ELF'
    subprocess.run([os.environ['MUSL_CC'], '-static', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-O2', source, '-o', guest], check=True)
    probe = output / 'probe'
    subprocess.run([sys.executable, ROOT / 'scripts/make_uefi_probe.py', '--build', args.build,
                    '--output', probe, '--display-width', '800', '--display-height', '600'],
                   env=dict(os.environ, BROOK_INITRD_KIB='8192'), check=True)
    initrd = probe / 'payload/INITRD.IMG'
    startup = output / 'INIT.RC'
    startup.write_text('set wm\nset vfb none\nrun --console --uid 0 /boot/FILL.ELF\nreboot\n')
    subprocess.run(['mcopy', '-i', initrd, guest, '::/BOOT/FILL.ELF'], check=True)
    subprocess.run(['mcopy', '-o', '-i', initrd, startup, '::/BOOT/INIT.RC'], check=True)
    disk = probe / 'probe.raw'
    subprocess.run(['mcopy', '-o', '-i', f'{disk}@@1048576', initrd, '::INITRD.IMG'], check=True)
    variables = output / 'VARS.fd'
    shutil.copyfile(os.environ['OVMF_VARS'], variables)
    serial = output / 'serial.log'
    command = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm' if kvm else 'tcg',
               '-cpu', 'host' if kvm else 'max', '-smp', '4', '-m', '768',
               '-nodefaults', '-display', 'none', '-nic', 'none', '-no-reboot',
               '-drive', f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
               '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
               '-device', 'qemu-xhci,id=usb',
               '-drive', f'if=none,id=stick,format=raw,readonly=on,file={disk}',
               '-device', 'usb-storage,bus=usb.0,drive=stick,bootindex=1',
               '-serial', f'file:{serial}']
    with (output / 'qemu.log').open('w') as log:
        vm = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(vm, serial, 'VOLATILE_FULL_PASS', 90)
            assert vm.wait(timeout=30) == 0, 'checked reset did not exit QEMU cleanly'
            boot_log = serial.read_text(errors='replace')
            assert 'volatile desktop /tmp ready' in boot_log
            assert 'first sector write failure' not in boot_log
            assert 'shutdown: final sync vfs=0' in boot_log
            assert 'phase=ready result=0' in boot_log and 'phase=committing result=0' in boot_log
        finally:
            if vm.poll() is None:
                vm.terminate()
                try:
                    vm.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    vm.kill()
                    vm.wait()
    print('VOLATILE_SHUTDOWN_QEMU_PASS')


if __name__ == '__main__':
    main()

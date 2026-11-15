#!/usr/bin/env python3
"""QEMU stress for pipe poll subscription lifetime at a full fd table.

Run inside nix-shell after building Release. Builds a disposable RAM-only
probe image with src/tests/test_pipe_poll_stress_guest, boots it headless
under KVM (no display window, no network, no audio) and waits for
PIPE_POLL_STRESS_OK, a kernel panic or a timeout on the serial log.
"""
import argparse, json, os, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def run(*args, **kw):
    subprocess.run(list(map(str, args)), check=True, **kw)


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(10); self.s.connect(str(path))
        self.f = self.s.makefile('rwb', buffering=0); self.f.readline(); self.call('qmp_capabilities')

    def call(self, cmd):
        self.f.write((json.dumps({'execute': cmd}) + '\n').encode())
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r: return r['return']
            if 'error' in r: raise RuntimeError(r)

    def close(self):
        self.f.close(); self.s.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New directory for the disposable image and logs')
    parser.add_argument('--build', type=Path, default=root / 'build/release')
    parser.add_argument('--seconds', type=int, default=60, help='guest stress duration')
    parser.add_argument('--smp', default='4')
    parser.add_argument('--heap-poison', action='store_true', help='run with heap poison on (faster use-after-free detection)')
    parser.add_argument('--copies', type=int, default=1, help='concurrent stress processes')
    parser.add_argument('--timeout', type=float, default=0, help='default: seconds + 120')
    args = parser.parse_args()
    base = args.output.resolve(); base.mkdir(parents=True, exist_ok=False)
    elf = base / 'PIPESTRS.ELF'
    run(os.environ['MUSL_CC'], '-static', '-Wall', '-Wextra', '-Werror', '-O2',
        root / 'src/tests/test_pipe_poll_stress_guest/main.c', '-o', elf, '-lpthread')
    probe = base / 'probe'
    env = dict(os.environ, BROOK_INITRD_KIB='8192')
    run('python3', root / 'scripts/make_uefi_probe.py', '--build', args.build, '--output', probe,
        '--display-width', '800', '--display-height', '600', env=env)
    initrd = base / 'INITRD.IMG'; shutil.copy2(probe / 'payload/INITRD.IMG', initrd)
    run('mcopy', '-i', initrd, elf, '::BOOT/PIPESTRS.ELF')
    rc = base / 'INIT.RC'; rc.write_text('set wm\nset vfb none\nkconsole\n' + ('heap poison on\n' if args.heap_poison else '') +
                  ''.join(f'run /boot/PIPESTRS.ELF {args.seconds}\n' for _ in range(args.copies)))
    run('mcopy', '-o', '-i', initrd, rc, '::BOOT/INIT.RC')
    disk = probe / 'probe.raw'
    run('mcopy', '-o', '-i', str(disk) + '@@1048576', initrd, '::INITRD.IMG')
    vars_fd = base / 'VARS.fd'; shutil.copy2(os.environ['OVMF_VARS'], vars_fd); vars_fd.chmod(0o600)
    qmp_dir = tempfile.TemporaryDirectory(prefix='brook-qmp-'); qmp_path = Path(qmp_dir.name) / 'qmp.sock'
    serial = base / 'serial.log'
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host', '-smp', args.smp, '-m', '2048',
            '-nodefaults', '-drive', 'if=pflash,format=raw,readonly=on,file=' + os.environ['OVMF_CODE'],
            '-drive', 'if=pflash,format=raw,file=' + str(vars_fd), '-device', 'VGA', '-device', 'qemu-xhci,id=usbc',
            '-drive', 'if=none,id=stick,format=raw,readonly=on,file=' + str(disk),
            '-device', 'usb-storage,bus=usbc.0,drive=stick,bootindex=1', '-display', 'none',
            '-serial', 'file:' + str(serial), '-qmp', 'unix:' + str(qmp_path) + ',server=on,wait=off',
            '-no-reboot', '-nic', 'none']
    (base / 'argv.json').write_text(json.dumps(argv, indent=2))
    log = (base / 'qemu.log').open('x'); owner = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT); q = None
    verdict = None
    try:
        deadline = time.monotonic() + (args.timeout or args.seconds + 120)
        while time.monotonic() < deadline:
            if owner.poll() is not None: verdict = 'QEMU ended early'; break
            if not q and qmp_path.exists(): q = Qmp(qmp_path)
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if 'PANIC_HEX:' in text or 'KERNEL PANIC' in text: verdict = 'kernel panic'; break
            if 'PIPE_POLL_STRESS_FAIL' in text: verdict = 'guest failure'; break
            if 'PIPE_POLL_STRESS_OK' in text: verdict = 'pass'; break
            time.sleep(.2)
        else:
            verdict = 'timeout'
    finally:
        if owner.poll() is None:
            try:
                if q: q.call('quit'); owner.wait(timeout=20)
                else: raise RuntimeError('no QMP')
            except BaseException:
                owner.terminate(); owner.wait(timeout=20)
        if q: q.close()
        log.close(); qmp_dir.cleanup()
    text = serial.read_text(errors='replace') if serial.exists() else ''
    lines = [l for l in text.splitlines() if 'PIPE_POLL_STRESS' in l or 'PANIC' in l or 'exhausted' in l][-40:]
    result = {'verdict': verdict, 'lines': lines}
    (base / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    for line in lines: print(line)
    print('PIPE_POLL_STRESS_QEMU', verdict.upper().replace(' ', '_'), flush=True)
    return 0 if verdict == 'pass' else 1


if __name__ == '__main__':
    sys.exit(main())

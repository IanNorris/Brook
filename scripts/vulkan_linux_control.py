#!/usr/bin/env python3
"""Diskless, headless Linux Venus control; never attaches Brook/user disks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import subprocess
import time


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', type=Path, default=Path('build/vulkan-linux-control-result'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--host-icd', type=Path, required=True)
    parser.add_argument('--rendernode', type=Path, default=Path('/dev/dri/renderD128'))
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    if not 30 <= args.timeout <= 600:
        parser.error('--timeout must be 30–600 seconds')
    control = args.control.resolve(strict=True)
    packages = json.loads((control / 'manifest.json').read_text())
    icd = args.host_icd.resolve(strict=True)
    node = args.rendernode.resolve(strict=True)
    if not stat.S_ISCHR(node.stat().st_mode):
        parser.error('rendernode must be a character device')
    # A single control VM avoids contention and memory pressure. /proc must be
    # fully visible to this launcher; use its normal host permission context.
    for proc in Path('/proc').glob('[0-9]*/exe'):
        try:
            target = str(proc.resolve(strict=True))
        except (OSError, RuntimeError):
            continue
        if 'qemu-system' in Path(target).name:
            raise RuntimeError(f'A QEMU VM is already running: {proc.parent.name}')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = output / 'runtime'
    runtime.mkdir(mode=0o700)
    qemu = Path(packages['qemu']) / 'bin/qemu-system-x86_64'
    serial = output / 'serial.log'
    qmp = output / 'qmp.sock'
    argv = [str(qemu), '-no-user-config', '-nodefaults', '-no-reboot',
            '-machine', 'q35,memory-backend=mem', '-accel', 'kvm', '-cpu', 'host',
            '-smp', '8', '-m', '4G',
            '-object', 'memory-backend-memfd,id=mem,size=4G,share=on', '-vga', 'none',
            '-device', 'virtio-gpu-gl-pci,id=vgpu,blob=true,venus=true,hostmem=1G',
            '-display', f'egl-headless,rendernode={node}',
            '-serial', f'file:{serial}', '-qmp', f'unix:{qmp},server=on,wait=off',
            '-kernel', str(control / 'bzImage'), '-initrd', str(control / 'initrd'),
            '-append', 'console=ttyS0 rdinit=/init panic=-1']
    environment = dict(os.environ)
    for name in ('DISPLAY', 'WAYLAND_DISPLAY'):
        environment.pop(name, None)
    environment.update(VK_DRIVER_FILES=str(icd), XDG_RUNTIME_DIR=str(runtime),
                       GBM_BACKENDS_PATH=packages['mesa'] + '/lib/gbm',
                       LIBGL_DRIVERS_PATH=packages['mesa'] + '/lib/dri',
                       __EGL_VENDOR_LIBRARY_DIRS=packages['mesa'] + '/share/glvnd/egl_vendor.d',
                       LD_LIBRARY_PATH=':'.join(packages[key] + '/lib'
                                                for key in ('virglrenderer', 'glvnd', 'mesa', 'vulkanLoader')))
    manifest = dict(packages=packages, argv=argv, host_kernel=platform.release(),
                    rendernode=str(node), pci_device=(Path('/sys/class/drm') / node.name / 'device').resolve().name,
                    host_icd=str(icd), host_icd_json=json.loads(icd.read_text()),
                    environment={key: environment[key] for key in
                                 ('VK_DRIVER_FILES', 'XDG_RUNTIME_DIR', 'GBM_BACKENDS_PATH',
                                  'LIBGL_DRIVERS_PATH', '__EGL_VENDOR_LIBRARY_DIRS', 'LD_LIBRARY_PATH')},
                    images=dict(kernel_sha256=sha(control / 'bzImage'), initrd_sha256=sha(control / 'initrd')),
                    revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                    diskless=True, guest_credentials='root in disposable Linux control', timeout_seconds=args.timeout)
    write(output / 'manifest.json', manifest)
    (output / 'source.patch').write_bytes(subprocess.check_output(['git', 'diff']))
    start = time.monotonic()
    timed_out = False
    with (output / 'qemu.stdout').open('wb') as stdout, (output / 'qemu.stderr').open('wb') as stderr:
        process = subprocess.Popen(argv, env=environment, stdout=stdout, stderr=stderr)
        write(output / 'launcher.json', dict(pid=process.pid, started_host_monotonic=start))
        print('LINUX_VENUS_CONTROL_STARTED', process.pid, str(output), flush=True)
        expected_renderer=(Path(packages['virglrenderer']) / 'lib/libvirglrenderer.so.1').resolve()
        renderer_maps=[]
        for attempt in range(40):
            try:
                renderer_maps=[line.split()[-1] for line in Path(f'/proc/{process.pid}/maps').read_text().splitlines()
                               if 'libvirglrenderer.so' in line]
            except OSError:
                break
            if renderer_maps or process.poll() is not None:
                break
            time.sleep(0.05)
        renderer_verified=bool(renderer_maps) and all(Path(path).resolve()==expected_renderer for path in renderer_maps)
        write(output / 'renderer-library.json',dict(expected=str(expected_renderer),
              sha256=sha(expected_renderer),loaded=sorted(set(renderer_maps)),verified=renderer_verified))
        try:
            exit_code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            # Only this owned, diskless control is stopped on timeout. Preserve
            # logs and mark failure; never reinterpret a forced stop as clean.
            process.terminate()
            try:
                exit_code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                exit_code = process.wait()
    text = serial.read_text(errors='replace') if serial.exists() else ''
    ended = re.findall(r'BROOK_VULKAN_CONTROL_END pci=(\d+) gpu=(\d+) drm=(\d+) info=(\d+) probe=(\d+) context=(\d+) blob=(\d+)', text)
    records = []
    for line in text.splitlines():
        if line.startswith('{"passed":'):
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    renderer_errors = [line for line in (text + (output / 'qemu.stderr').read_text(errors='replace')).splitlines()
                       if re.search(r'\[drm:.*\*ERROR\*|context error|failed to dispatch|'
                                    r'virtio_gpu_virgl_process_cmd.*error|failed to open libvulkan', line)]
    passed = (renderer_verified and not timed_out and not renderer_errors and exit_code == 0 and len(ended) == 1
              and all(int(value) == 0 for value in ended[0])
              and len(records) == 2 and all(record['passed'] for record in records)
              and records[-1].get('driver_id') == 22 and records[-1].get('rounds') == 3)
    result = dict(passed=passed, qemu_exit=exit_code, timed_out=timed_out,
                  seconds=time.monotonic() - start, end_markers=ended, probes=records,
                  serial_sha256=sha(serial) if serial.exists() else None,
                  full_output_in_files=True, guest_poweroff_marker_seen=bool(ended),
                  renderer_errors=renderer_errors)
    write(output / 'result.json', result)
    print(json.dumps(result), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())

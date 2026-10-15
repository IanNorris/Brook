#!/usr/bin/env python3
"""Eight-vCPU I/O integrity/performance test on isolated RAM or SSD images."""
import argparse
import json
import os
from pathlib import Path
import re
import random
import shutil
import socket
import subprocess
import tempfile
import time
import bootstrap_acceptance as base

ROOT = base.ROOT

def host_memory():
    memory = Path('/proc/meminfo').read_text()
    vmstat = Path('/proc/vmstat').read_text()
    return {'available_bytes': int(re.search(r'^MemAvailable:\s+(\d+)', memory, re.M)[1])*1024,
            'swap_pages': {key: int(re.search(r'^'+key+r' (\d+)', vmstat, re.M)[1])
                           for key in ('pswpin', 'pswpout')}}


def allocation(image):
    p = subprocess.run(['dumpe2fs', '-h', str(image)], capture_output=True, text=True, check=True)
    return {key: int(re.search(r'^'+key+r':\s+(\d+)', p.stdout, re.M)[1])
            for key in ('Free blocks', 'Free inodes')}

def check(directory, output, tag):
    for name in ('data', 'nix'):
        with (output / f'{tag}-{name}-fsck.log').open('wb') as f:
            rc = subprocess.run(['e2fsck', '-fn', str(directory / f'{name}.img')], stdout=f, stderr=subprocess.STDOUT).returncode
        if rc:
            raise RuntimeError(f'{tag}: {name} fsck returned {rc}')
    return allocation(directory / 'data.img')

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--backend', choices=['ram', 'disk'], default='ram')
    ap.add_argument('--artifacts', type=Path, default=ROOT / 'build/release')
    ap.add_argument('--cpus', type=int, default=8)
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--mib', type=int, default=8)
    ap.add_argument('--files', type=int, default=64)
    ap.add_argument('--instance', type=int, help='VNC display number; default chooses a free local port')
    ap.add_argument('--cache-probe', action='store_true', help='also run eight-process random pread/mmap integrity checks')
    ap.add_argument('--lifetime-probe', action='store_true', help='also run the 24 inode-lifetime regression checks')
    ap.add_argument('--deadline', type=int, default=180, help='seconds allowed per guest workload')
    args = ap.parse_args()
    if args.instance is None:
        candidates = list(range(120, 500))
        random.SystemRandom().shuffle(candidates)
        for instance in candidates:
            try:
                with socket.socket() as vnc, socket.socket() as debug:
                    vnc.bind(('127.0.0.1', 5900+instance))
                    debug.bind(('127.0.0.1', 11237+100*instance))
                args.instance = instance
                break
            except OSError:
                continue
        else: ap.error('No free VM endpoint pair')
    if not 1 <= args.instance <= 500:
        ap.error('instance must be 1..500 for valid QEMU forwarding ports')
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'build') or output.exists():
        ap.error('output must be a new directory below build/')
    if args.cpus < 8 or not 1 <= args.workers <= 16 or not 8 <= args.mib <= 128 or not 1 <= args.files <= 1024:
        ap.error('need >=8 CPUs, 1..16 workers, 8..128 MiB/worker, 1..1024 files/worker')
    output.mkdir(parents=True)
    parent = Path('/dev/shm') if args.backend == 'ram' else output
    if args.backend == 'ram' and (shutil.disk_usage(parent).free < 3*1024**3):
        raise RuntimeError('RAM filesystem needs at least 3 GiB free')
    if args.backend == 'ram' and host_memory()['available_bytes'] < 4*1024**3:
        raise RuntimeError('RAM-backed test needs at least 4 GiB available host memory')
    directory = Path(tempfile.mkdtemp(prefix='brook-io-', dir=parent))
    process = session = serial = None
    success = False
    report = {'settings': vars(args) | {'output': str(output), 'artifacts': str(args.artifacts)},
              'host': base.host_environment(), 'memory_before': host_memory(), 'runtime_directory': str(directory),
              'kernel_sha256': base.sha(args.artifacts / 'kernel/BROOK.elf'), 'phases': []}
    report['settings'] = {k: str(v) if isinstance(v, Path) else v for k,v in report['settings'].items()}
    base.write(output / 'environment.json', report)
    start = time.monotonic()
    try:
        for name, size in [('data', max(2048, args.workers*args.mib+512)), ('nix', 64)]:
            seed = directory / f'{name}-seed'; seed.mkdir()
            (seed / 'BROOK.MNT').write_text('/'+name)
            image = directory / f'{name}.img'
            with image.open('wb') as f: f.truncate(size*1024*1024)
            base.command(['mkfs.ext2', '-q', '-F', '-b', '4096', '-d', str(seed), str(image)], output / f'format-{name}.log')
        baseline = check(directory, output, 'initial')
        artifacts = directory / 'artifacts'
        for subpath in ('kernel/BROOK.elf', 'bootloader/BOOTX64.efi', *[str(p.relative_to(args.artifacts)) for p in (args.artifacts/'kernel/drivers').glob('*.mod')]):
            target = artifacts / subpath
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.artifacts/subpath, target)
        env = dict(os.environ, BROOK_DISK_IMG=str(directory/'boot.img'), BROOK_BUILD_DIR=str(artifacts))
        base.command([str(ROOT/'scripts/create_disk.sh')], output/'boot-create.log', env)
        base.command([str(ROOT/'scripts/update_disk.sh')], output/'boot-update.log', env)
        base.command(['mcopy', '-o', '-i', str(directory/'boot.img'), str(ROOT/'build/io-bench/io-bench'), '::BIN/IOBENCH'])
        if args.cache_probe:
            base.command(['mcopy', '-o', '-i', str(directory/'boot.img'), str(ROOT/'build/io-bench/cachecheck'), '::BIN/CACHECHK'])
        if args.lifetime_probe:
            base.command(['mcopy', '-o', '-i', str(directory/'boot.img'), str(ROOT/'build/package-cache/filelifecheck'), '::BIN/FILELIFE'])
        for mode in ('run', 'cleanup'):
            process, session, data, boot_seconds = base.launch(directory, args.instance, artifacts, cpus=args.cpus, memory='2G')
            serial = Path(data['serial_log'])
            offset = serial.stat().st_size
            reply = base.server.guest_run(session, f'run /boot/BIN/IOBENCH /data/io-test {mode} {args.workers} {args.mib} {args.files}')
            base.write(output/f'{mode}-launch.json', reply)
            expires = time.monotonic() + args.deadline
            while time.monotonic() < expires:
                content = serial.read_bytes()[offset:].decode(errors='replace')
                if 'IOBENCH FAIL' in content or re.search(r'PANIC|KERNEL FAULT|virtio-blk.*(?:timeout|reset)', content, re.I):
                    raise RuntimeError(f'{mode}: guest I/O failure; see serial log')
                if f'IOBENCH_PASS mode={mode}' in content: break
                if process.poll() is not None: raise RuntimeError('VM exited during test')
                time.sleep(.2)
            else: raise TimeoutError(f'{mode}: benchmark exceeded {args.deadline}s')
            if args.cache_probe and mode == 'run':
                base.server.guest_run(session, f'run /boot/BIN/CACHECHK /data/io-test {args.workers} {args.mib}')
                expires = time.monotonic()+args.deadline
                while time.monotonic()<expires:
                    extra = serial.read_bytes()[offset:].decode(errors='replace')
                    if 'CACHECHECK FAIL' in extra: raise RuntimeError('Concurrent read/mmap corruption')
                    if 'CACHECHECK PASS' in extra: break
                    time.sleep(.2)
                else: raise TimeoutError('Concurrent cache probe deadline')
            if args.lifetime_probe and mode == 'cleanup':
                base.server.guest_run(session, 'run /boot/BIN/FILELIFE /data --require-reuse')
                expires = time.monotonic() + args.deadline
                while time.monotonic() < expires:
                    extra = serial.read_bytes()[offset:].decode(errors='replace')
                    if 'FILELIFECHECK FAIL' in extra or 'FILELIFECHECK FATAL' in extra:
                        raise RuntimeError('Inode lifetime regression')
                    if 'FILELIFECHECK PASS failures=0' in extra: break
                    time.sleep(.2)
                else: raise TimeoutError('Inode lifetime probe deadline')
            # Program synchronizes before PASS. Stop waits for another guest sync.
            stop = base.server.control(session, 'stop')
            base.write(output/f'{mode}-stop.json', stop)
            process.wait(timeout=30); process = session = None
            final_serial = serial.read_bytes()[offset:].decode(errors='replace')
            if re.search(r'IOBENCH FAIL|CACHECHECK FAIL|FILELIFECHECK FAIL|PANIC|KERNEL FAULT|virtio-blk.*(?:timeout|reset)', final_serial, re.I):
                raise RuntimeError(f'{mode}: late guest failure during verification or sync')
            shutil.copy2(serial, output/f'{mode}-serial.log')
            shutil.copy2(directory/'launch.log', output/f'{mode}-host.log')
            hostlog = (output/f'{mode}-host.log').read_text(errors='replace')
            if re.search(r'virtio.*(?:timeout|error)|PANIC', hostlog, re.I):
                raise RuntimeError(f'{mode}: host device errors')
            if args.cache_probe and mode == 'run':
                values = re.findall(r'CACHECHECK_RESULT (\{[^\n]+\})', final_serial)
                if len(values) != 1: raise RuntimeError('Missing cache probe timing')
                report['cache_probe'] = json.loads(values[0])
            phases = [json.loads(m) for m in re.findall(r'IOBENCH_RESULT (\{[^\n]+\})', content)]
            if len(phases) != (4 if mode == 'run' else 2): raise RuntimeError('Missing phase results')
            counts = check(directory, output, mode)
            for phase in phases:
                phase['mib_per_second'] = phase['bytes']/(1024*1024)/(phase['total_ns']/1e9)
            report['phases'].extend(phases)
            report[mode] = {'boot_seconds': boot_seconds, 'allocation': counts}
        if counts != baseline: raise RuntimeError(f'Allocation leak: baseline={baseline}, final={counts}')
        report.update(passed=True, memory_after=host_memory(), elapsed_seconds=time.monotonic()-start, initial_allocation=baseline)
        base.write(output/'result.json', report)
        success = True
        print(json.dumps(report, indent=2))
    finally:
        if not success:
            base.write(output/'failure.json', {'passed': False, 'runtime_directory': str(directory), 'session': session})
            if serial and serial.exists(): shutil.copy2(serial, output/'failure-serial.log')
            if (directory/'launch.log').exists(): shutil.copy2(directory/'launch.log', output/'failure-host.log')
        if session:
            try: base.server.control(session, 'stop')
            except Exception: pass
        if process:
            try: process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                # Leave images untouched if launcher is still alive.
                print(f'VM still running; preserved {directory}', flush=True)
                raise RuntimeError(f'VM still running; images preserved at {directory}')
        if success:
            shutil.rmtree(directory)
        else:
            # Failed images stay available for diagnosis; never run repair automatically.
            print(f'Failed test images preserved at {directory}', flush=True)

if __name__ == '__main__': main()

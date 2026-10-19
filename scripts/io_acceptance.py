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
    for name in ('data', 'nix', *(['tmp'] if (directory / 'tmp.img').exists() else [])):
        with (output / f'{tag}-{name}-fsck.log').open('wb') as f:
            rc = subprocess.run(['e2fsck', '-fn', str(directory / f'{name}.img')], stdout=f, stderr=subprocess.STDOUT).returncode
        if rc:
            raise RuntimeError(f'{tag}: {name} fsck returned {rc}')
    if (directory/'boot.img').exists():
        with (output/f'{tag}-boot-fsck.log').open('wb') as f:
            rc = subprocess.run(['fsck.fat', '-n', str(directory/'boot.img')], stdout=f, stderr=subprocess.STDOUT).returncode
        if rc: raise RuntimeError(f'{tag}: boot FAT fsck returned {rc}')
    return allocation(directory / 'data.img')


def wait_flush(process, serial, offset, marker, deadline):
    expires = time.monotonic() + deadline
    while time.monotonic() < expires:
        text = serial.read_bytes()[offset:].decode(errors='replace')
        if re.search(r'FLUSHCHECK FAIL|KERNEL PANIC|KERNEL FAULT', text):
            raise RuntimeError('Flush regression: '+text[-2000:])
        if (marker.search(text) if isinstance(marker, re.Pattern) else marker in text): return text
        if process.poll() is not None: raise RuntimeError('VM exited during flush probe')
        time.sleep(.1)
    raise TimeoutError('Flush probe deadline: '+str(marker))


def flush_probe(process, session, data, serial, artifacts, output, deadline, mount, slot):
    import inject_flush_failure
    path = mount+'/flush-check'
    offset = serial.stat().st_size
    base.server.guest_run(session, 'run /boot/BIN/FLUSHCHK create '+path)
    wait_flush(process, serial, offset, 'FLUSHCHECK PASS path='+path, deadline)
    offset = serial.stat().st_size
    base.server.guest_run(session, 'run /boot/BIN/FLUSHCHK retry '+path+' --wait')
    text = wait_flush(process, serial, offset, re.compile(r'FLUSHCHECK READY pid=\d+ path='), deadline)
    pid = int(re.search(r'FLUSHCHECK READY pid=(\d+)', text)[1])
    # This isolated harness attaches boot/data/nix in that exact order. Check
    # the driver's own slot announcement rather than guessing another VM's.
    if f'virtio-blk: virtio{slot} flush=negotiated test-budget-slot={slot}' not in serial.read_text():
        raise RuntimeError('Unexpected data-device slot or missing negotiated FLUSH')
    first = inject_flush_failure.inject(session, artifacts/'kernel/BROOK.elf', slot=slot, count=1)
    base.server.guest_run(session, f'run /boot/BIN/FLUSHCHK release {pid}')
    wait_flush(process, serial, offset, 'FLUSHCHECK PASS path='+path, deadline)
    second = inject_flush_failure.inject(session, artifacts/'kernel/BROOK.elf', slot=slot, count=1)
    try:
        base.server.control(session, 'stop')
    except RuntimeError as error:
        if 'aborted before QMP' not in str(error): raise
        rejected = str(error)
    else: raise RuntimeError('Failed device flush incorrectly permitted VM shutdown')
    if process.poll() is not None: raise RuntimeError('Negative sync acknowledgement stopped VM')
    with inject_flush_failure.QMP(session) as qmp:
        if qmp.call('query-status').get('running') is not True:
            raise RuntimeError('VM did not remain running after rejected shutdown')
    retry = base.server.guest(data, 'bridge sync', timeout=30)
    if retry != {'guest_sync_completed': True, 'sync_guarantee': 'checked_filesystem_and_device_flush'}:
        raise RuntimeError('Checked sync retry failed: '+str(retry))
    result = {'fsync_injection': first, 'shutdown_injection': second,
              'shutdown_rejected': rejected, 'sync_retry': retry}
    base.write(output/f'flush-failure-tests-{slot}.json', result)
    return result

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--mixed-append-probe', nargs='?', const='all', default='all', choices=['all','unlink','truncate','chain'])
    ap.add_argument('--block-size', type=int, choices=[1024,2048,4096], default=4096)
    ap.add_argument('--backend', choices=['ram', 'disk'], default='ram')
    ap.add_argument('--artifacts', type=Path, default=ROOT / 'build/release')
    ap.add_argument('--memory', choices=['2G','4G','8G'], default='2G')
    ap.add_argument('--host-headroom-gib', type=int, choices=[1,2,4,8], default=4,
                    help='host memory left available beyond guest/test images; default 4 GiB')
    ap.add_argument('--cpus', type=int, default=8)
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--mib', type=int, default=8)
    ap.add_argument('--files', type=int, default=64)
    ap.add_argument('--instance', type=int, help='VNC display number; default chooses a free local port')
    ap.add_argument('--cache-probe', action='store_true', help='also run eight-process random pread/mmap integrity checks')
    ap.add_argument('--wait-mask-probe', action='store_true', help='also run temporary-mask, child-signal and select regression probes')
    ap.add_argument('--profile-small', action='store_true', help='capture CPU/context switches during small-files; timings are instrumented')
    ap.add_argument('--lifetime-probe', action='store_true', help='also run the 24 inode-lifetime regression checks')
    ap.add_argument('--flush-probe', action='store_true', help='inject device flush failures, require fsync/stop errors and retry, then verify after reboot')
    ap.add_argument('--thread-page-probe', action='store_true', help='retain worker mappings after thread exit and require safe PID reuse by unrelated processes')
    ap.add_argument('--exit-group-probe', nargs='?', const=128, type=int, help='race thread-group exit with sleeping/waking workers for this many rounds')
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
    if args.exit_group_probe is not None and not 8 <= args.exit_group_probe <= 1024:
        ap.error('exit-group-probe must use 8..1024 rounds')
    output.mkdir(parents=True)
    parent = Path('/dev/shm') if args.backend == 'ram' else output
    if args.backend == 'ram' and (shutil.disk_usage(parent).free < 3*1024**3):
        raise RuntimeError('RAM filesystem needs at least 3 GiB free')
    required_memory = (int(args.memory[:-1])+args.host_headroom_gib+(1 if args.backend == 'ram' else 0))*1024**3
    if host_memory()['available_bytes'] < required_memory:
        raise RuntimeError(f'Test needs {required_memory/1024**3:.0f} GiB available host memory for guest, image allowance and configured headroom')
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
            if name == 'data':
                # Kernel home initialization persists these five directories.
                # Include them before the allocation baseline, so the final
                # exact comparison measures workload leaks rather than boot.
                for relative in ('home/.cache', 'home/.config', 'home/.local/share'):
                    (seed / relative).mkdir(parents=True, exist_ok=True)
            image = directory / f'{name}.img'
            with image.open('wb') as f: f.truncate(size*1024*1024)
            base.command(['mkfs.ext2', '-q', '-F', '-b', str(args.block_size), '-d', str(seed), str(image)], output / f'format-{name}.log')
        baseline = check(directory, output, 'initial')
        artifacts = directory / 'artifacts'
        for subpath in ('kernel/BROOK.elf', 'kernel/ksym_addr_table.cpp', 'bootloader/BOOTX64.efi', *[str(p.relative_to(args.artifacts)) for p in (args.artifacts/'kernel/drivers').glob('*.mod')]):
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
        if args.wait_mask_probe:
            for source, target in [('waitmaskcheck','WAITMASK'),('childsignalcheck','CHLDSIG'),('selectcheck','SELECTCHK')]:
                base.command(['mcopy', '-o', '-i', str(directory/'boot.img'), str(ROOT/'build/io-bench'/source), '::BIN/'+target])
        if args.mixed_append_probe:
            base.command(['mcopy', '-o', '-i', str(directory/'boot.img'), str(ROOT/'build/io-bench/mixedappendcheck'), '::BIN/MIXAPP'])
        if args.flush_probe:
            base.command(['mcopy', '-o', '-i', str(directory/'boot.img'), str(ROOT/'build/io-bench/flushcheck'), '::BIN/FLUSHCHK'])
        if args.thread_page_probe:
            base.command(['mcopy', '-o', '-i', str(directory/'boot.img'), str(ROOT/'build/io-bench/threadpagecheck'), '::BIN/ASOWNCHK'])
        if args.exit_group_probe:
            base.command(['mcopy', '-o', '-i', str(directory/'boot.img'), str(ROOT/'build/io-bench/exitgroupcheck'), '::BIN/EXITGRP'])
        for mode in ('run', 'cleanup'):
            process, session, data, boot_seconds = base.launch(directory, args.instance, artifacts, cpus=args.cpus, memory=args.memory)
            serial = Path(data['serial_log'])
            offset = serial.stat().st_size
            if args.exit_group_probe and mode == 'run':
                before_exit = base.server.guest(data, 'bridge stats')['process_count']
                base.server.guest_run(session, f'run /boot/BIN/EXITGRP {args.exit_group_probe}')
                expires = time.monotonic()+args.deadline
                while time.monotonic()<expires:
                    text = serial.read_bytes()[offset:].decode(errors='replace')
                    if re.search(r'EXITGROUPCHECK FAIL|KERNEL PANIC|KERNEL FAULT', text):
                        raise RuntimeError('Thread-group exit regression')
                    if f'EXITGROUPCHECK PASS rounds={args.exit_group_probe}' in text: break
                    if process.poll() is not None: raise RuntimeError('VM exited during exit-group probe')
                    time.sleep(.1)
                else: raise TimeoutError('Exit-group probe deadline')
                reap_deadline = time.monotonic()+10
                while True:
                    after_exit = base.server.guest(data, 'bridge stats')['process_count']
                    if after_exit <= before_exit: break
                    if time.monotonic() >= reap_deadline:
                        raise RuntimeError(f'Worker records leaked after exit stress: before={before_exit}, after={after_exit}')
                    time.sleep(.1)
                report['exit_group_probe'] = {'rounds': args.exit_group_probe, 'passed': True,
                                              'processes_before': before_exit, 'processes_after': after_exit}
            if args.thread_page_probe and mode == 'run':
                base.server.guest_run(session, 'run /boot/BIN/ASOWNCHK /data --require-reuse')
                expires = time.monotonic()+args.deadline
                while time.monotonic()<expires:
                    text = serial.read_bytes()[offset:].decode(errors='replace')
                    if re.search(r'THREADPAGECHECK FAIL|KERNEL PANIC|KERNEL FAULT', text):
                        raise RuntimeError('Thread-owned mappings corrupted after PID reuse')
                    if 'THREADPAGECHECK PASS failures=0' in text: break
                    if process.poll() is not None: raise RuntimeError('VM exited during thread page probe')
                    time.sleep(.1)
                else: raise TimeoutError('Thread page probe deadline')
                report['thread_page_probe'] = 'passed with observed TID reuse'
            if args.flush_probe:
                if mode == 'run':
                    report['flush_probe'] = {
                        mount: flush_probe(process, session, data, serial, artifacts, output, args.deadline, mount, slot)
                        for mount, slot in [('/data', 1), ('/boot', 0)]}
                else:
                    for mount in ['/data', '/boot']:
                        probe_offset = serial.stat().st_size
                        base.server.guest_run(session, 'run /boot/BIN/FLUSHCHK cleanup '+mount+'/flush-check')
                        wait_flush(process, serial, probe_offset, 'FLUSHCHECK CLEANUP PASS', args.deadline)
            if args.mixed_append_probe and mode == 'run':
                base.server.guest_run(session, 'run /boot/BIN/MIXAPP /data '+args.mixed_append_probe)
                expires = time.monotonic()+args.deadline
                while time.monotonic()<expires:
                    text = serial.read_bytes()[offset:].decode(errors='replace')
                    if 'MIXAPPEND FAIL' in text or re.search(r'KERNEL PANIC|KERNEL FAULT',text):
                        raise RuntimeError('Mixed append cache regression')
                    if 'MIXAPPEND PASS mode=' in text: break
                    if process.poll() is not None: raise RuntimeError('VM exited during mixed append probe')
                    time.sleep(.2)
                else: raise TimeoutError('Mixed append probe deadline')
                report['mixed_append_probe'] = args.mixed_append_probe
            reply = base.server.guest_run(session, f'run /boot/BIN/IOBENCH /data/io-test {mode} {args.workers} {args.mib} {args.files}')
            base.write(output/f'{mode}-launch.json', reply)
            small_profile_started = False
            expires = time.monotonic() + args.deadline
            while time.monotonic() < expires:
                content = serial.read_bytes()[offset:].decode(errors='replace')
                if 'IOBENCH FAIL' in content or re.search(r'PANIC|KERNEL FAULT|virtio-blk.*(?:timeout|reset)', content, re.I):
                    raise RuntimeError(f'{mode}: guest I/O failure; see serial log')
                if args.profile_small and mode == 'run' and not small_profile_started and 'IOBENCH_BEGIN small-files' in content:
                    report['small_profile_start'] = base.server.profile(session, 'start', duration_ms=3000)
                    small_profile_started = True
                if f'IOBENCH_PASS mode={mode}' in content: break
                if process.poll() is not None: raise RuntimeError('VM exited during test')
                time.sleep(.2)
            else: raise TimeoutError(f'{mode}: benchmark exceeded {args.deadline}s')
            if small_profile_started:
                profile_deadline = time.monotonic()+15
                while 'RUNNING' in base.server.profile(session, 'status').get('reply', ''):
                    if time.monotonic()>profile_deadline: raise TimeoutError('Small-file profile did not finish')
                    time.sleep(.2)
                report['small_profile'] = base.server.profile(session, 'export')
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
            if args.wait_mask_probe and mode == 'cleanup':
                for binary, marker in [('WAITMASK','WAITMASKCHECK'),('CHLDSIG','CHILDSIGNALCHECK'),('SELECTCHK','SELECTCHECK')]:
                    probe_offset = serial.stat().st_size
                    base.server.guest_run(session, 'run /boot/BIN/'+binary+(' 10.0.2.15' if binary == 'SELECTCHK' else ''))
                    expires = time.monotonic()+args.deadline
                    while time.monotonic()<expires:
                        extra = serial.read_bytes()[probe_offset:].decode(errors='replace')
                        if marker+' FAIL' in extra or re.search(r'KERNEL PANIC|KERNEL FAULT',extra):
                            raise RuntimeError(marker+' guest regression')
                        if marker+' PASS failures=0' in extra: break
                        if process.poll() is not None: raise RuntimeError('VM exited during '+marker)
                        time.sleep(.2)
                    else: raise TimeoutError(marker+' deadline')
                    report.setdefault('guest_probes', []).append(marker)
            # Program synchronizes before PASS. Stop waits for another guest sync.
            stop = base.server.control(session, 'stop')
            if args.flush_probe and stop.get('sync_guarantee') != 'checked_filesystem_and_device_flush':
                raise RuntimeError('Shutdown lost checked flush guarantee')
            base.write(output/f'{mode}-stop.json', stop)
            process.wait(timeout=30); process = session = None
            final_serial = serial.read_bytes()[offset:].decode(errors='replace')
            if re.search(r'IOBENCH FAIL|CACHECHECK FAIL|FILELIFECHECK FAIL|PANIC|KERNEL FAULT|BRO176-MAP(?:LEAK|UNDERFLOW)|SCHED: exit_group timeout|virtio-blk.*(?:timeout|reset)', final_serial, re.I):
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

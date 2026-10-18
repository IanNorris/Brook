#!/usr/bin/env python3
"""Decompose a real cached package's transfer/decompression/extraction on Brook.

Run in nix-shell with no VM live. Disks and host HTTP server are private; failures
retain the same VM/request for inspection rather than treating a timeout as exit.
"""
import argparse
from decimal import Decimal
import functools
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

import bootstrap_acceptance as b
sys.path.insert(0, str(b.ROOT/'scripts/workloads'))
from package_pipeline_verify import inventory


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--seed', type=Path, required=True)
    ap.add_argument('--narinfo', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--pcap', action='store_true', help='instrument this run with a private QEMU packet capture')
    ap.add_argument('--save-images', action='store_true', help='archive successful disposable images; default releases them after checked shutdown/fsck')
    ap.add_argument('--rounds', type=int, choices=(1, 2), default=2)
    ap.add_argument('--instance', type=int, default=247)
    ap.add_argument('--deadline', type=int, default=900)
    args = ap.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(b.ROOT/'build') or output.exists():
        ap.error('--output must be a new directory under build/')
    if any(s.get('attachable') for s in b.server.sessions()['sessions']):
        ap.error('Stop other VMs before this performance run')
    seed = args.seed.resolve()
    seed_manifest = json.loads((seed/'manifest.json').read_text())
    if seed_manifest.get('bootstrap_mode') != 'prepared':
        ap.error('A prepared SDK seed is required')
    fields = dict(line.split(': ', 1) for line in args.narinfo.read_text().splitlines() if ': ' in line)
    archive = (args.narinfo.parent/fields['URL']).resolve()
    if not archive.is_relative_to(args.narinfo.parent.resolve()) or fields['Compression'] != 'zstd':
        ap.error('Expected a local zstd NAR within the cache directory')
    if archive.stat().st_size != int(fields['FileSize']):
        ap.error('Compressed NAR size mismatch')
    checksum = subprocess.check_output(['nix-hash', '--flat', '--base32', '--type', 'sha256', str(archive)], text=True).strip()
    if fields['FileHash'] != 'sha256:'+checksum:
        ap.error('Compressed NAR hash mismatch')
    seed_bytes = sum((seed/f'{name}.img').stat().st_blocks*512 for name in ('data', 'nix'))
    image_budget = seed_bytes + int(fields['NarSize'])*12 + 1024**3
    required = image_budget + 8*1024**3  # Four GiB guest + four GiB host reserve.
    if b.memory_available() < required or shutil.disk_usage('/dev/shm').free < image_budget:
        ap.error('Insufficient available memory/tmpfs for private payload and guest')
    output.mkdir(parents=True)
    payload = output/'http';payload.mkdir()
    shutil.copyfile(archive, payload/'archive.zst')
    with (payload/'archive.nar').open('wb') as dest:
        subprocess.run(['zstd', '-dq', '-c', str(archive)], stdout=dest, check=True)
    if (payload/'archive.nar').stat().st_size != int(fields['NarSize']):
        raise RuntimeError('Uncompressed NAR size mismatch')
    checksum = subprocess.check_output(['nix-hash', '--flat', '--base32', '--type', 'sha256', str(payload/'archive.nar')], text=True).strip()
    if fields['NarHash'] != 'sha256:'+checksum:
        raise RuntimeError('Uncompressed NAR hash mismatch')
    reference = output/'reference'
    with (payload/'archive.nar').open('rb') as stream:
        subprocess.run(['nix-store', '--restore', str(reference)], stdin=stream, check=True)
    expected = {'inventory': inventory(reference), 'nar_sha256': b.sha(payload/'archive.nar'),
                'compressed_sha256': b.sha(payload/'archive.zst')}
    b.write(output/'expected.json', expected)
    ram = Path(tempfile.mkdtemp(prefix='brook-pipeline-', dir='/dev/shm'))
    for name in ('data', 'nix', 'boot'):
        (output/f'{name}.img').symlink_to(ram/f'{name}.img')
    (ram/'tmp-host').mkdir();(output/'tmp-host').symlink_to(ram/'tmp-host')
    for name in ('data', 'nix'):
        b.command(['cp', '--sparse=always', str(seed/f'{name}.img'), str(ram/f'{name}.img')])
    artifacts = output/'artifacts'
    for part in ('kernel/BROOK.elf', 'kernel/ksym_addr_table.cpp', 'bootloader/BOOTX64.efi',
                 *[str(p.relative_to(b.ROOT/'build/release')) for p in (b.ROOT/'build/release/kernel/drivers').glob('*.mod')]):
        target = artifacts/part;target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(b.ROOT/'build/release'/part, target)
    env = dict(os.environ, BROOK_DISK_IMG=str(output/'boot.img'), BROOK_BUILD_DIR=str(artifacts))
    b.command([str(b.ROOT/'scripts/create_disk.sh')], output/'create-boot.log', env)
    b.command([str(b.ROOT/'scripts/update_disk.sh')], output/'update-boot.log', env)
    commands = output/'inject.txt'
    commands.write_text('mkdir /pipeline\n'+''.join(f'write {src} /pipeline/{name}\n' for src, name in [
        (payload/'archive.nar', 'archive.nar'), (payload/'archive.zst', 'archive.zst'), (output/'expected.json', 'expected.json')]))
    b.command(['debugfs', '-w', '-f', str(commands), str(output/'data.img')], output/'inject.log')
    b.command([os.environ['MUSL_CC'], '-O2', '-Wall', '-Wextra', '-static',
        str(b.ROOT/'tools/io-bench/monotime.c'), '-o', str(output/'monotime')])
    b.command(['mcopy', '-o', '-i', str(output/'boot.img'), str(output/'monotime'), '::PIPECLK'])
    for src, dest in [('package_pipeline.sh', 'PIPELINE.SH'), ('package_pipeline_verify.py', 'PIPECHK')]:
        b.command(['mcopy', '-o', '-i', str(output/'boot.img'), str(b.ROOT/'scripts/workloads'/src), '::'+dest])
    before = b.offline_fsck(output, 'before')
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(payload))
    cache = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=cache.serve_forever, daemon=True).start()
    metadata = {'narinfo': fields, 'seed': seed_manifest, 'host': b.host_environment(),
                'guest': {'cpus': 8, 'memory': '4G', 'audio': 'none'}, 'ram_directory': str(ram),
                'kernel_sha256': b.sha(artifacts/'kernel/BROOK.elf'),
                'driver_sha256': {p.name: b.sha(p) for p in (artifacts/'kernel/drivers').glob('*.mod')},
                'timer_sha256': b.sha(output/'monotime'), 'before_fsck': before}
    b.write(output/'manifest.json', metadata)
    process, session, data, boot_seconds = b.launch(output, args.instance, artifacts, cpus=8, memory='4G')
    qemu_argv = [p.decode() for p in Path(f"/proc/{data['pid']}/cmdline").read_bytes().split(b'\0') if p]
    metadata.update(session=session, boot_seconds=boot_seconds, qemu_argv=qemu_argv,
                    packet_capture_instrumented=args.pcap, images_retained=args.save_images)
    if args.pcap:
        devices = [qemu_argv[i+1] for i, item in enumerate(qemu_argv[:-1]) if item == '-netdev']
        user = [v for v in devices if v.startswith('user,')]
        if len(user) != 1:raise RuntimeError('Expected one private user-network backend')
        netdev = re.search(r'(?:^|,)id=([^,]+)', user[0]).group(1)
        with b.server.QMP(session) as qmp:
            qmp.call('object-add', {'qom-type': 'filter-dump', 'id': 'pipeline-capture',
                'netdev': netdev, 'file': str(ram/'network.pcap')})

    b.write(output/'manifest.json', metadata)
    print('READY', session, flush=True)
    job = b.TrackedCommand(output, session, ['/nix/profile/bin/brook-bootstrap-shell', '/boot/PIPELINE.SH',
        '/data/pipeline', f'http://10.0.2.2:{cache.server_port}/archive.zst', str(args.rounds)])
    started = time.monotonic();seen = 0
    while time.monotonic()-started < args.deadline:
        text = job.poll()
        lines = text.splitlines()
        for line in lines[seen:]:
            if line.startswith(('PIPELINE_', '{')):print(line, flush=True)
        seen = len(lines)
        if job.complete():break
        if b.server.panic_state(data):raise RuntimeError('Guest panic; retain VM and private disks')
        time.sleep(.5)
    else:raise TimeoutError('Retain same VM/request; observation deadline is not termination')
    assert 'PIPELINE_ALL_PASS' in text
    results = [{'stage': name, 'guest_seconds': float(Decimal(end)-Decimal(start))}
        for name, start, end in re.findall(r'^PIPELINE_TIME (\S+) ([0-9.]+) ([0-9.]+)$', text, re.M)]
    assert len(results) == args.rounds*11 and all(r['guest_seconds'] >= 0 for r in results)
    metadata['host_observed_seconds'] = time.monotonic()-started
    metadata['heap'] = b.server.guest(data, 'heap check')
    assert metadata['heap'].get('reply') == 'heap integrity: OK'
    stop = b.server.control(session, 'stop');assert stop.get('guest_sync_completed')
    metadata['stop'] = stop
    process.wait(timeout=30)
    assert process.returncode == 0 and not b.server.process_is_alive(data['pid'], data['process_identity'])
    b.check_stopped_serial(data, output)
    metadata['after_fsck'] = b.offline_fsck(output, 'after')
    cache.shutdown()
    if args.pcap:shutil.copyfile(ram/'network.pcap', output/'network.pcap')
    for name in ('boot', 'data', 'nix'):
        image = output/f'{name}.img'
        if args.save_images:
            saved = output/f'{name}.saved.img'
            b.command(['cp', '--sparse=always', str(image), str(saved)])
            image.unlink();saved.rename(image)
        else:image.unlink()
    (output/'tmp-host').unlink();(output/'tmp-host').mkdir()
    shutil.rmtree(ram)
    metadata.update(success=True, timings=results)
    b.write(output/'result.json', metadata)
    print('PIPELINE_ACCEPTANCE_PASS', json.dumps(results), flush=True)


if __name__ == '__main__':
    main()

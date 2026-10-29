#!/usr/bin/env python3
"""Assemble a clean portable candidate from an explicit public-profile seed.

Run in nix-shell. Existing guest/review disks are never inputs. The generated
candidate stays private until the exact corresponding-source/notice review is
complete; open-source package selection alone is not release clearance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MIB = 1024 * 1024


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 * MIB), b''):
            value.update(chunk)
    return value.hexdigest()


def copy_extents(source, destination, offset):
    """Only copy allocated extents into a newly created zero-filled disk."""
    with source.open('rb') as src, destination.open('r+b') as dst:
        end, cursor = source.stat().st_size, 0
        while cursor < end:
            try:
                start = os.lseek(src.fileno(), cursor, os.SEEK_DATA)
            except OSError as exc:
                if exc.errno == 6:  # ENXIO: remainder is a hole
                    break
                raise
            stop = min(end, os.lseek(src.fileno(), start, os.SEEK_HOLE))
            src.seek(start)
            dst.seek(offset + start)
            while start < stop:
                chunk = src.read(min(4 * MIB, stop - start))
                if not chunk:
                    raise RuntimeError('Unexpected sparse source EOF')
                dst.write(chunk)
                start += len(chunk)
            cursor = stop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    args = parser.parse_args()
    seed, output, build = args.seed.resolve(), args.output.resolve(), args.build.resolve()
    metadata = json.loads((seed / 'manifest.json').read_text())
    if metadata.get('public_inputs_only') is not True or metadata.get('profile_expression') != str(ROOT / 'nix/release-packages.nix'):
        parser.error('Requires fresh --public --profile-expression nix/release-packages.nix seed')
    output.mkdir(parents=True, exist_ok=False)
    stage = output / 'assembly'
    (stage / 'build/kernel/drivers').mkdir(parents=True)
    for module in (build / 'kernel/drivers').glob('*.mod'):
        shutil.copy2(module, stage / 'build/kernel/drivers' / module.name)
    env = dict(os.environ, BROOK_BUILD_DIR=str(stage / 'build'), BROOK_INITRD_KIB='32768')
    env.pop('BROOK_PROVIDER_PAYLOAD', None)
    run('bash', ROOT / 'scripts/make_initrd.sh', '--release', env=env)
    # Assemble from an allowlist, not build/esp or any prior boot disk.
    esp = stage / 'esp.img'
    with esp.open('xb') as stream:
        stream.truncate(128 * MIB)
    run('mkfs.fat', '-F', '32', '-n', 'BROOK_EFI', esp)
    run('mmd', '-i', esp, '::EFI', '::EFI/BOOT', '::KERNEL', '::BIN')
    script = stage / 'INIT.RC'
    script.write_text('set wm\nset vfb none\n'
                      'run --service --once --uid 1000 --gid 100 /nix/profile/bin/brook-dbus --config-file=/nix/profile/bin/brook-session.conf --nofork --address=unix:path=/tmp/brook-session-bus --nosyslog\n'
                      'run --service --once --quiet --uid 1000 --gid 100 /nix/profile/bin/brook-message --watch\n'
                      'run --service --once /nix/profile/bin/waylandd\n')
    # Brook deliberately skips the GPT ESP during kernel filesystem mounting.
    # Keep the early /boot script and terminal shell in the loaded RAM disk.
    initrd = stage / 'build/esp/INITRD.IMG'
    run('mmd', '-i', initrd, '::BOOT', '::BOOT/BIN', '::BOOT/SHORTCUTS')
    run('mcopy', '-i', initrd, script, '::BOOT/INIT.RC')
    run('mcopy', '-i', initrd, Path(metadata['profile']) / 'bin/bash', '::BOOT/BIN/BASH')
    run('mcopy', '-i', initrd, ROOT / 'data/wallpaper.raw', '::BOOT/WALLPAPER.RAW')
    cfg = stage / 'BROOK.CFG'
    cfg.write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=0\nLOG_MEMORY=0\nLOG_INTERRUPTS=0\n')
    marker = stage / 'BROOK.MNT'
    marker.write_text('/boot')
    exports = [(build / 'bootloader/BOOTX64.efi', '::EFI/BOOT/BOOTX64.EFI'),
               (build / 'kernel/BROOK.elf', '::KERNEL/BROOK.ELF'),
               (stage / 'build/esp/INITRD.IMG', '::INITRD.IMG'),
               (script, '::INIT.RC'), (cfg, '::BROOK.CFG'), (marker, '::BROOK.MNT'),
               (Path(metadata['profile']) / 'bin/bash', '::BIN/BASH')]
    for source, target in exports:
        run('mcopy', '-i', esp, source, target)
    # Finalization is offline on fresh private copies; never mount or modify a
    # running VM disk. Include helpers in the guest as well as the host bundle.
    data_image, nix_image = stage / 'data.img', stage / 'nix.img'
    for source, target in [(seed / 'data.img', data_image), (seed / 'nix.img', nix_image)]:
        run('cp', '--sparse=always', '--reflink=auto', source, target)
    commands = stage / 'data-debugfs.txt'
    lines = ['mkdir /distribution', 'mkdir /distribution/installers']
    for source in sorted((ROOT / 'distribution/installers').iterdir()):
        if source.is_file() and source.suffix in ('.py', '.sh'):
            lines.append(f'write "{source}" /distribution/installers/{source.name}')
    commands.write_text('\n'.join(lines) + '\n')
    run('debugfs', '-w', '-f', commands, data_image)
    idx = subprocess.check_output(['debugfs', '-R', 'cat /share/applications/applications.idx', str(nix_image)], text=True)
    # A terminal-only .desktop entry cannot be opened directly by our GUI.
    entries = [line for line in idx.splitlines() if not line.startswith('Vim\t')]
    for number, line in enumerate(entries):
        fields = line.split('\t')
        if fields[0] == 'VLC media player':
            fields[1] = '--uid 1000 /nix/profile/bin/brook-vlc'
            entries[number] = '\t'.join(fields)
        elif fields[0] in ('GNU Image Manipulation Program', 'Mousepad', 'OpenTTD', 'Qalculate!', 'Gnome Commander', 'GNOME Commander'):
            # Ensure generic upstream desktop entries get the same profile-
            # relative software graphics paths as browser/media wrappers.
            fields[1] = '--uid 1000 /nix/profile/bin/brook-run ' + fields[1]
            entries[number] = '\t'.join(fields)
    public_idx = stage / 'applications.idx'
    public_idx.write_text('\n'.join(entries) + '\n')
    commands.write_text(f'rm /share/applications/applications.idx\nwrite "{public_idx}" /share/applications/applications.idx\n')
    run('debugfs', '-w', '-f', commands, nix_image)
    for image in (data_image, nix_image):
        run('e2fsck', '-fn', image)
    # AF_UNIX endpoints require an ext2-backed /tmp; the early FAT RAM disk
    # cannot host Wayland or D-Bus sockets. Keep it separate from user files.
    tmp_tree, tmp_image = stage / 'tmp', stage / 'tmp.img'
    tmp_tree.mkdir()
    (tmp_tree / 'BROOK.MNT').write_text('/tmp')
    with tmp_image.open('xb') as stream:
        stream.truncate(512 * MIB)
    run('mkfs.ext2', '-F', '-b', '4096', '-d', tmp_tree, tmp_image)
    run('debugfs', '-w', '-R', 'set_inode_field / mode 041777', tmp_image)
    run('e2fsck', '-fn', tmp_image)
    parts = [esp, data_image, nix_image, tmp_image]
    starts, cursor = [], MIB
    for part in parts:
        starts.append(cursor)
        cursor += part.stat().st_size
        cursor = (cursor + MIB - 1) // MIB * MIB
    disk = stage / 'brook.raw'
    with disk.open('xb') as stream:
        stream.truncate(cursor + MIB)
    table = 'label: gpt\nunit: sectors\n\n' + ''.join(
        f'start={start // 512},size={part.stat().st_size // 512},type={kind},name="{name}"\n'
        for part, start, kind, name in zip(parts, starts, ['U', 'L', 'L', 'L'], ['Brook EFI', 'Brook data', 'Brook Nix', 'Brook tmp']))
    run('sfdisk', '--no-reread', '--no-tell-kernel', disk, input=table, text=True)
    for part, start in zip(parts, starts):
        copy_extents(part, disk, start)
    run('qemu-img', 'convert', '-f', 'raw', '-O', 'qcow2', '-c', disk, output / 'brook.qcow2')
    run('qemu-img', 'compare', '-f', 'raw', '-F', 'qcow2', disk, output / 'brook.qcow2')
    run('qemu-img', 'check', output / 'brook.qcow2')
    for name, variable in [('OVMF_CODE.fd', 'OVMF_CODE'), ('OVMF_VARS.fd', 'OVMF_VARS')]:
        path = Path(os.environ[variable])
        shutil.copy2(path, output / name)
    for name in ['launch.py', 'mcp.py', 'run-linux.sh', 'run-macos.command', 'run-windows.cmd', 'README.txt']:
        shutil.copy2(ROOT / 'tools/portable_qemu' / name, output / name)
    shutil.copy2(ROOT / 'distribution/software.json', output / 'software.json')
    shutil.copy2(ROOT / 'LICENSE', output / 'BROOK-LICENSE.txt')
    shutil.copytree(ROOT / 'tools/vm_bridge', output / 'linux-development-bridge',
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    shutil.copytree(ROOT / 'distribution/installers', output / 'installers',
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.gitignore'))
    manifest = dict(format=1, status='private-candidate-source-and-notice-review-pending',
                    profile=metadata.get('profile'), seed_manifest=metadata,
                    revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                    build_info=(build / 'kernel/build_info_generated.cpp').read_text(),
                    kernel_sha256=digest(build / 'kernel/BROOK.elf'),
                    bootloader_sha256=digest(build / 'bootloader/BOOTX64.efi'),
                    module_sha256={p.name: digest(p) for p in sorted((build / 'kernel/drivers').glob('*.mod'))},
                    partitions=[dict(offset=start, size=part.stat().st_size, sha256=digest(part)) for part, start in zip(parts, starts)],
                    files={name: digest(output / name) for name in ['brook.qcow2', 'OVMF_CODE.fd', 'OVMF_VARS.fd']},
                    acceptance=dict(linux='pending', windows='not tested', macos_intel='not tested', macos_arm='not tested; TCG only'))
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Candidate:', output, flush=True)


if __name__ == '__main__':
    main()

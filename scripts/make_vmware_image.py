#!/usr/bin/env python3
"""Build a fresh guarded GPT SATA VMDK and VMX for a persistent Brook desktop.

No devices are opened and no VM is started. Run inside the pinned Nix shell.
The operator policy contains public Ed25519 keys. The checker source supplies
only the two static executables already pinned by Brook's recovery kernel.
"""

import argparse
import gzip
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

from bridge_keyless_image import require_keyless_disk, require_keyless_image
from bridge_operator_policy import validate_operator_policy
from build_dell_image import build_bridge_objects, build_provenance, bridge_compression
from hardware_recovery_fsck import validate_checker
from make_public_qemu import copy_extents
from prepare_guard_check_pair import CHECKER_SHA, E2FSCK_SHA
from prepare_hardware_store import digest
from stage_clearwater_shell import add_clearwater_start, embed_clearwater_executables, embed_terminal_shortcut

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_update_protocol import record

SECTOR = 512
MIB = 1024 * 1024
ESP_FIRST = 2048
ESP_SECTORS = 512 * MIB // SECTOR
DATA_FIRST = ESP_FIRST + ESP_SECTORS
ESP_TYPE = 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'
DATA_TYPE = '0fc63daf-8483-4772-8e79-3d69d8477de4'
PROFILE_EXPR = ROOT / 'nix/hardware-network-packages.nix'
KERNEL_FLAGS = (
    'BROOK_SSD_GUARD_CHECK', 'BROOK_SSD_AUTO_RECOVERY', 'BROOK_SSD_GUARDED_DESKTOP',
    'BROOK_SSD_GUARDED_DESKTOP_RESET', 'BROOK_VM_PERSISTENT_AHCI', 'BROOK_BRIDGE_V3',
)
DESKTOP_RC = b'''set wm
set vfb none
sched eevdf
run --service --once --console --uid 1000 /nix/profile/bin/hardware-dbus
run --service --once --console --uid 1000 /nix/profile/bin/hardware-message
run --service --once --console /nix/profile/bin/waylandd
run --service --once --console /boot/HWSTATUS.ELF
'''
RECOVERY_RC = b'set wm\nset vfb none\nkconsole\nrun --service --once --console /boot/HWSTATUS.ELF\n'


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], cwd=ROOT, check=True, **kwargs)


def output(*args):
    return run(*args, capture_output=True, text=True).stdout.strip()


def fat_write(image, path, data):
    run('mcopy', '-o', '-i', image, '-', '::/' + path, input=data)
    if run('mtype', '-i', image, '::/' + path, capture_output=True).stdout != data:
        raise ValueError('FAT readback mismatch for ' + path)


def get_checkers(source, work):
    opener = gzip.open if source.suffix == '.gz' else open
    with opener(source, 'rb') as stream:
        content = stream.read(64 * MIB + 1)
    if not 0 < len(content) <= 64 * MIB:
        raise ValueError('Checker source must be a bounded standalone initrd')
    payload = work / 'checker-source.img'
    payload.write_bytes(content)
    result = {}
    for name, expected in (('FATFSCK.ELF', CHECKER_SHA), ('E2FSCK.ELF', E2FSCK_SHA)):
        path = work / name
        path.write_bytes(run('mtype', '-i', payload, '::/BOOT/' + name, capture_output=True).stdout)
        if validate_checker(path)['sha256'] != expected:
            raise ValueError(name + ' differs from the recovery kernel pin')
        result['BOOT/' + name] = path.read_bytes()
    payload.unlink()
    return result


def boot_helpers(build, work, policy_path):
    vendor, objects = build_bridge_objects(build)
    cc = os.environ['MUSL_CC']
    cxx = os.environ.get('MUSL_CXX') or cc.removesuffix('gcc') + 'g++'
    common = ('-static', '-O2', '-Wall', '-Wextra', '-Werror')
    bridge = ('-DBROOK_BRIDGE_V3', f'-DBROOK_BRIDGE_V3_POLICY_SHA256="{digest(policy_path)}"', '-I', vendor)
    zlib_flags, zlib_archive = bridge_compression()
    bulk = ('tools/hardware_bulk.c',)
    update = ('tools/hardware_update.c',)
    recipes = (
        ('HWBULK.ELF', cc, (), bulk, ()),
        ('HWBULKZ.ELF', cc, tuple(zlib_flags), bulk, (zlib_archive,)),
        ('HWSTAGE.ELF', cc, ('-DBROOK_BULK_INSTALL_MEDIA',), bulk, ()),
        ('HWUPDATE.ELF', cc, (), update, ()),
        ('HWUPSYS.ELF', cc, ('-DBROOK_UPDATE_SYSTEM',), update, ()),
        ('HWSTORE.ELF', cxx, ('-std=c++17', '-Wno-misleading-indentation'),
         ('tools/hardware_store_receiver.cpp', 'tools/hardware_store_transaction.cpp',
          'tools/hardware_store_validate.cpp'), ()),
    )
    payload = {}
    for name, compiler, flags, sources, libraries in recipes:
        binary = work / name
        run(compiler, *common, *bridge, *flags, *sources, *objects, *libraries, '-o', binary)
        payload['BOOT/' + name] = binary.read_bytes()
    for name, source in (('HWSTATUS.ELF', 'tools/hardware_status.c'),
                         ('BRKMODE.ELF', 'tools/hardware_ram_exec_stage.c')):
        run(cc, *common, source, '-o', work / name)
    payload['BOOT/HWSTATUS.ELF'] = (work / 'HWSTATUS.ELF').read_bytes()
    return payload


def build_kernel(work, policy):
    build = work / 'kernel'
    run('cmake', '-S', ROOT / 'src/kernel', '-B', build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/toolchains/kernel-x86_64-clang.cmake'),
        *('-D' + name + ':BOOL=ON' for name in KERNEL_FLAGS),
        '-DBROOK_BRIDGE_V3_OPERATOR_KEYS_FILE:FILEPATH=' + str(policy))
    run('cmake', '--build', build)
    return build


def make_initrds(work, build, payload, policy, profile, commit, name):
    tree = work / 'initrd'
    modules = tree / 'kernel/drivers'
    modules.mkdir(parents=True)
    shutil.copy2(build / 'drivers/e1000.mod', modules / 'e1000.mod')
    env = dict(os.environ, BROOK_BUILD_DIR=str(tree), BROOK_INITRD_KIB='16384')
    for key in ('BROOK_PROVIDER_PAYLOAD', 'BROOK_INITRD_MARKERS', 'BROOK_HARDWARE_PAYLOAD_ID'):
        env.pop(key, None)
    run('bash', ROOT / 'scripts/make_initrd.sh', '--release', env=env)
    base = tree / 'esp/INITRD.IMG'
    for directory in ('::/BOOT', '::/BOOT/BIN', '::/MEDIA', '::/MEDIA/SYSTEM', '::/DATA', '::/NIX'):
        if subprocess.run(['mdir', '-i', base, directory], capture_output=True).returncode:
            run('mmd', '-i', base, directory)
    provenance, _ = build_provenance(commit, False, None)
    for path, data in {**payload, 'ETC/bridge-v3-operators': policy.read_bytes(),
                       'ETC/hardware-maintenance': b'1\n',
                       'ETC/build-provenance': provenance}.items():
        fat_write(base, path, data)

    desktop = work / 'desktop-initrd.img'
    recovery = work / 'recovery-initrd.img'
    shutil.copyfile(base, desktop)
    shutil.copyfile(base, recovery)
    supervisor = Path(output('nix-build', '--no-out-link',
                             ROOT / 'tools/vulkan-compositor-pkg/clearwater-supervisor.nix'))
    shell = Path(output('nix-build', '--no-out-link', ROOT / 'tools/clearwater-shell-pkg/default.nix'))
    embed_clearwater_executables(desktop, supervisor / 'bin/CWSUP.ELF', shell / 'bin/CWSHELL.ELF')
    embed_terminal_shortcut(desktop)
    # The guard binds persistent home under the desktop's initial RAM /data.
    fat_write(desktop, 'ETC/volatile-desktop', b'1\n')
    fat_write(desktop, 'ETC/hardware-bulk', b'1\n')
    fat_write(desktop, 'BOOT/INIT.RC', add_clearwater_start(DESKTOP_RC))
    fat_write(recovery, 'BOOT/INIT.RC', RECOVERY_RC)
    fat_write(desktop, 'BOOT/BIN/BASH', (profile / 'bin/bash').read_bytes())
    for image, suffix in ((desktop, ''), (recovery, '-recovery')):
        fat_write(image, 'ETC/hardware-payload-id', (name + suffix + '\n').encode())
        require_keyless_image(image)
    return desktop, recovery


def guard_record(disk, data, filesystem):
    result = bytearray(128)
    result[:8] = b'BRKRCV01'
    struct.pack_into('<QI', result, 8, 1, 1)
    result[24:72] = disk.bytes_le + data.bytes_le + filesystem.bytes
    struct.pack_into('<I', result, 124, zlib.crc32(result[:124]))
    return bytes(result)


def assemble_disk(work, data_image, efi, kernel, desktop, recovery, disk_id, esp_id, data_id, fs_id):
    data_count = data_image.stat().st_size // SECTOR
    total = DATA_FIRST + data_count + 2048
    raw = work / 'brook.raw'
    with raw.open('xb') as file:
        file.truncate(total * SECTOR)
    layout = (f'label: gpt\nlabel-id: {disk_id}\nunit: sectors\n\n'
              f'start={ESP_FIRST}, size={ESP_SECTORS}, type={ESP_TYPE}, uuid={esp_id}, name="Brook EFI"\n'
              f'start={DATA_FIRST}, size={data_count}, type={DATA_TYPE}, uuid={data_id}, name="Brook data"\n')
    run('sfdisk', '--no-reread', '--no-tell-kernel', raw, input=layout, text=True)
    run('sfdisk', '--verify', raw)

    esp = work / 'esp.img'
    with esp.open('xb') as file:
        file.truncate(ESP_SECTORS * SECTOR)
    # dosfstools rounds the volume down to a whole track. Set a track
    # size that divides the ESP so the FAT and GPT sector counts agree.
    run('mkfs.fat', '-F', '32', '-g', '64/32', '-n', 'BROOK_EFI', esp)
    with esp.open('rb') as file:
        boot_sector = file.read(SECTOR)
    if struct.unpack_from('<I', boot_sector, 32)[0] != ESP_SECTORS:
        raise ValueError('FAT boot sector must cover the exact GPT ESP')
    for name in ('EFI', 'EFI/BOOT', 'RECOVERY', 'RECOV2', 'UPDATES', 'UPDATES/A', 'UPDATES/B'):
        run('mmd', '-i', esp, '::/' + name)
    fat_write(esp, 'EFI/BOOT/BOOTX64.EFI', efi.read_bytes())
    fat_write(esp, 'EFI/BOOT/BRKMODE.ELF', (work / 'BRKMODE.ELF').read_bytes())
    # The normal payload lives in UPDATES/A and UPDATES/B.
    fat_write(esp, 'BROOK.CFG', b'TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=0\nLOG_MEMORY=0\nUPDATE_SLOTS=1\n')
    fat_write(esp, 'PANIC.BIN', bytes(258 * SECTOR))
    guard = guard_record(disk_id, data_id, fs_id)
    for copy in (0, 1):
        fat_write(esp, f'RECOVERY/GUARD{copy}.BIN', guard)
    kdata, ndata, rdata = kernel.read_bytes(), desktop.read_bytes(), recovery.read_bytes()
    embedded_images = []
    for directory in ('RECOV2', 'RECOVERY'):
        fat_write(esp, directory + '/KERNEL.ELF', kdata)
        fat_write(esp, directory + '/INITRD.IMG', rdata)
        embedded_images.append((directory + '/INITRD.IMG', recovery))
        fat_write(esp, directory + '/SELECT.BIN', record(1, 0, kdata, rdata))
    for slot, label in enumerate(('A', 'B')):
        fat_write(esp, f'UPDATES/{label}/KERNEL.ELF', kdata)
        fat_write(esp, f'UPDATES/{label}/INITRD.IMG', ndata)
        embedded_images.append((f'UPDATES/{label}/INITRD.IMG', desktop))
        fat_write(esp, f'UPDATES/SELECT{slot}.BIN', record(slot + 1, slot, kdata, ndata))
    run('fsck.fat', '-n', esp)
    require_keyless_image(esp, embedded_images=embedded_images)
    copy_extents(esp, raw, ESP_FIRST * SECTOR)
    copy_extents(data_image, raw, DATA_FIRST * SECTOR)
    return raw


def vmx(network):
    lines = ['.encoding = "UTF-8"', 'config.version = "8"', 'virtualHW.version = "20"',
             'displayName = "Brook"', 'guestOS = "other-64"', 'firmware = "efi"',
             'uefi.secureBoot.enabled = "FALSE"', 'memsize = "4096"', 'numvcpus = "2"',
             'mks.enable3d = "FALSE"', 'devices.hotplug = "FALSE"', 'pciBridge0.present = "TRUE"']
    for port in range(4, 8):
        lines += [f'pciBridge{port}.present = "TRUE"', f'pciBridge{port}.virtualDev = "pcieRootPort"',
                  f'pciBridge{port}.functions = "8"']
    lines += ['sata0.present = "TRUE"', 'sata0:0.present = "TRUE"', 'sata0:0.fileName = "brook.vmdk"',
              'ethernet0.present = "TRUE"', 'ethernet0.virtualDev = "e1000e"',
              f'ethernet0.connectionType = "{network}"', 'ethernet0.startConnected = "TRUE"',
              'ethernet0.addressType = "generated"', 'sound.present = "FALSE"',
              'serial0.present = "TRUE"', 'serial0.fileType = "file"', 'serial0.fileName = "serial.log"',
              'serial0.startConnected = "TRUE"']
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--operator-policy', type=Path, required=True)
    parser.add_argument('--recovery-checkers-from', type=Path, required=True,
                        help='Standalone recovery initrd containing the pinned static FAT and ext2 checkers')
    parser.add_argument('--network', choices=('hostonly', 'nat'), default='hostonly')
    parser.add_argument('--data-mib', type=int,
                        help='store capacity in MiB (default: 6144 for the compatibility profile)')
    parser.add_argument('--selection-config', type=Path,
                        help='host-side app selection JSON from brook_image.py')
    parser.add_argument('--payload-id', default='vmware-persistent')
    args = parser.parse_args()
    if args.data_mib is not None and not 1024 <= args.data_mib <= 32768:
        parser.error('data-mib must be between 1024 and 32768')
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,50}', args.payload_id):
        parser.error('Invalid payload ID')
    if 'MUSL_CC' not in os.environ:
        parser.error('Run inside the pinned Nix shell')
    if output('git', 'status', '--porcelain', '--untracked-files=no'):
        parser.error('Build from a clean source commit')
    commit = output('git', 'rev-parse', 'HEAD')
    policy = args.operator_policy.read_bytes()
    validate_operator_policy(policy)
    checkers = args.recovery_checkers_from.resolve(strict=True)
    selection = None
    if args.selection_config:
        from image_profile import evaluate
        config = json.loads(args.selection_config.read_text())
        if config.get("target") != "vmware":
            parser.error("Selection target does not match VMware")
        selection = evaluate(config)
    dest = args.output.resolve()
    dest.mkdir(parents=True, exist_ok=False, mode=0o700)
    work = dest / 'assembly'
    work.mkdir()
    policy_path = work / 'operators.pub'
    policy_path.write_bytes(policy)

    profile_receipt = None
    build = None
    seed_args = []
    if selection is None:
        profile = Path(output('nix-build', '--no-out-link', PROFILE_EXPR, '-A', 'profile'))
        data_mib = args.data_mib or 6144
    else:
        from image_profile import closure_receipt, data_size_mib, realize
        from kernel_capabilities import read_kernel_capabilities, require_kernel_capabilities
        build = build_kernel(work, policy_path)
        kernel_capabilities = read_kernel_capabilities(build / 'BROOK.elf')
        require_kernel_capabilities(kernel_capabilities, selection['manifest']['requiredCapabilities'])
        profile = realize(config, selection)
        closure = closure_receipt(profile)
        data_mib = data_size_mib(closure['nar_bytes'], args.data_mib, 1024, 32768)
        selection_path = dest / 'profile-selection.json'
        selection_path.write_text(json.dumps(selection, indent=2, sort_keys=True) + '\n')
        seed_args = ['--profile-selection', selection_path]
        if args.data_mib is None:
            seed_args += ['--auto-size']
        profile_receipt = {'profile': str(profile), 'closure': closure,
                           'kernel': kernel_capabilities,
                           'redistributable': selection['manifest']['redistributable']}
    run(sys.executable, ROOT / 'scripts/prepare_hardware_store.py', '--profile', profile,
        '--direct-ahci', '--size-mib', data_mib, '--output', work / 'profile', *seed_args)
    data = work / 'profile/STORE.IMG'
    with data.open('rb') as file:
        file.seek(1024 + 104)
        fs_id = uuid.UUID(bytes=file.read(16))
    disk_id, esp_id, data_id = uuid.uuid4(), uuid.uuid4(), uuid.uuid4()
    data_count = data.stat().st_size // SECTOR
    total = DATA_FIRST + data_count + 2048
    esp_selector = f'BRKESP1\n{disk_id.bytes_le.hex()}\n{esp_id.bytes_le.hex()}\n{ESP_FIRST},{ESP_SECTORS}\n'
    data_selector = f'BRKDATA1\n{disk_id.bytes_le.hex()}\n{data_id.bytes_le.hex()}\n{DATA_FIRST},{data_count}\n{fs_id.hex}\n'
    private = work / 'guard-loader-options.txt'
    private.write_text(f'BROOK_GUARD_IDENTITY:STRING={disk_id.bytes_le.hex()}{data_id.bytes_le.hex()}{fs_id.hex}\n'
                       f'BROOK_GUARD_DATA_FIRST:STRING={DATA_FIRST}\nBROOK_GUARD_DATA_SECTORS:STRING={data_count}\n')
    private.chmod(0o600)
    records = {'ETC/ahci-observe': b'1\n', 'ETC/ahci-read-only': b'1\n',
               'ETC/ahci-vm-write-target': f'BRKAHCIVM1\n{total}\n'.encode(),
               'ETC/ahci-boot-volume': esp_selector.encode(), 'ETC/ahci-data-volume': data_selector.encode()}
    if build is None:
        build = build_kernel(work, policy_path)
    payload = {**get_checkers(checkers, work), **boot_helpers(build, work, policy_path), **records}
    desktop, recovery = make_initrds(work, build, payload, policy_path, profile, commit, args.payload_id)
    run(sys.executable, ROOT / 'scripts/build_dell_loader.py', '--shared', work,
        '--output', work / 'loader')
    efi, kernel = work / 'loader/BROOKGUARD.efi', build / 'BROOK.elf'
    raw = assemble_disk(work, data, efi, kernel, desktop, recovery, disk_id, esp_id, data_id, fs_id)
    require_keyless_disk(raw)
    vmdk = dest / 'brook.vmdk'
    run('qemu-img', 'convert', '-f', 'raw', '-O', 'vmdk', '-o', 'subformat=monolithicSparse', raw, vmdk)
    run('qemu-img', 'compare', '-f', 'raw', '-F', 'vmdk', raw, vmdk)
    require_keyless_disk(vmdk)

    (dest / 'Brook.vmx').write_text(vmx(args.network))
    report = {'version': 1, 'commit': commit, 'profile': str(profile), 'payload_id': args.payload_id,
              'network': args.network, 'disk_guid': str(disk_id), 'esp_guid': str(esp_id),
              'data_guid': str(data_id), 'ext2_uuid': str(fs_id), 'sectors': total,
              'esp_first': ESP_FIRST, 'esp_sectors': ESP_SECTORS, 'data_first': DATA_FIRST,
              'data_sectors': data_count, 'vmdk_sha256': digest(vmdk),
              'operator_policy_sha256': digest(policy_path), 'loader_sha256': digest(efi),
              'kernel_sha256': digest(kernel), 'desktop_initrd_sha256': digest(desktop),
              'recovery_initrd_sha256': digest(recovery)}
    if profile_receipt is not None:
        report['profile_selection'] = profile_receipt
    (dest / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    for path in (raw, data, work / 'esp.img'):
        path.unlink()
    print('Persistent VMware image prepared:', dest / 'Brook.vmx')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Build the generic RAM-only "desktop smoke" UEFI USB file set.

Target: a generic x86-64 UEFI PC booted once from USB (first use: the Z370 /
RTX 3080 desktop). The payload is derived from scripts/make_uefi_probe.py, not
from the Dell maintenance stick: the maintenance builder starts from the
private Dell source initrd, which carries identity-bound AHCI selectors.

What the payload binds or touches (everything else is never probed):
  * Loader: default bootloader build (no update slots, recovery menu, guard,
    boot-variable inventory). It reads its own volume; no EFI variable writes.
  * Kernel: Release kernel with BROOK_BRIDGE_V3=ON and the given operator
    public key(s); every Dell/SSD/AHCI-write/test option forced OFF. Built-in
    virtio-blk is the only built-in block driver (absent on real hardware).
    AHCI stays unbound: no /etc/ahci-* selector, no AHCI observation marker.
    There is no NVMe driver.
  * Modules: xhci only, driving every xHCI controller with USB 2/3 hubs
    (USB storage read-only via /etc/usb-read-only, USB HID boot keyboard and
    mouse, CDC-ECM USB Ethernet). No GPU, HDA or NIC PCI drivers.
  * /etc/hw-inventory: read-only boot inventory (CPU, memory map, ACPI table
    headers, MCFG, DMAR, PCI config space incl. ECAM extended capabilities).
  * /etc/boot-status: end-of-boot status block (every xHCI controller, its
    ports and devices incl. hubs, NICs, DHCP, bridge endpoint), re-printed on
    USB/network changes, and DHCP that keeps retrying on every NIC.
  * Root is the RAM initrd; nothing is mounted from any disk.

Writes only the new --output directory and --build-root. Run in nix-shell from
a clean checkout. Never pass a private seed: --operators takes public keys.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from bridge_keyless_image import require_keyless_disk, require_keyless_image  # noqa: E402
from bridge_operator_policy import validate_operator_policy  # noqa: E402

FORMAT = 'brook-rtx-smoke-v2'
FAT_LABEL = 'BROOK_RTX'
KERNEL_FLAGS = {
    'BROOK_BRIDGE_V3': 'ON', 'BROOK_QEMU_LEGACY_OUTBOUND_DEBUG': 'OFF',
    'BROOK_MAPPING_TEST_HOOKS': 'OFF', 'BROOK_REGISTER_TEST_HOOKS': 'OFF',
    'BROOK_GPU_CAPTURE_FAULT_TEST': 'OFF', 'BROOK_EFI_SHUTDOWN_TEST': 'OFF',
    'BROOK_DELL_ACPI_MONITOR': 'OFF', 'BROOK_AHCI_WRITE_TEST_HOOKS': 'OFF',
    'BROOK_AHCI_PANIC_TEST': 'OFF', 'BROOK_AHCI_PANIC_CONTENTION_TEST': 'OFF',
    'BROOK_SSD_INSTALLER': 'OFF', 'BROOK_SSD_GUARD_CHECK': 'OFF',
    'BROOK_SSD_GUARD_PENDING_TEST': 'OFF', 'BROOK_SSD_GUARD_EXT2_TEST': 'OFF',
    'BROOK_SSD_GUARD_COMPLETE_TEST': 'OFF', 'BROOK_SSD_AUTO_RECOVERY': 'OFF',
    'BROOK_VM_PERSISTENT_AHCI': 'OFF', 'BROOK_SSD_GUARDED_DESKTOP': 'OFF',
    'BROOK_SSD_GUARDED_DESKTOP_RESET': 'OFF', 'BROOK_SSD_GUARDED_DESKTOP_RESET_TEST': 'OFF',
    'BROOK_GPU_BLOCKED_RECOVERY_TEST': 'OFF', 'BROOK_BDW_CACHE_PROBE': 'OFF',
}
LOADER_FLAGS = {
    'BROOK_BUILD_BOOT_INVENTORY': 'OFF', 'BROOK_BUILD_GUARDED_BOOTLOADER': 'OFF',
    'BROOK_GUARD_CHECK_DATA': 'OFF', 'BROOK_GUARD_ALTERNATE_RECOVERY': 'OFF',
    'BROOK_BUILD_RECOVERY_PROBE': 'OFF',
}
DRIVERS = ('xhci',)
MARKERS = ('usb-read-only', 'audio-muted', 'hw-inventory', 'boot-status')
# Exact /etc contents allowed in the initrd (lower-case FAT names).
ETC_ALLOWED = {*MARKERS, 'hardware-payload-id', 'resolv.conf', 'machine-id', 'ssl'}
INIT_RC = b'set wm\nset vfb none\nkconsole\n'
LOADER_CONFIG = (b'TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nLOG_MEMORY=0\nLOG_INTERRUPTS=0\n'
                 b'UPDATE_SLOTS=0\nRECOVERY_MENU=0\nDISPLAY_WIDTH=1920\nDISPLAY_HEIGHT=1080\n')
README = '''Brook desktop smoke test (RAM only)

Boot once from the firmware boot menu (UEFI entry for this stick).
Secure Boot must be off for this unsigned development loader.
Brook runs from RAM: it does not mount, bind or write internal disks,
does not write EFI variables and does not change firmware settings.
USB storage is read-only. Remove the stick and reboot to return.
Bridge: v3 operator-authenticated TCP 1234 over a CDC-ECM USB NIC.
'''
NAMES = ('EFI/BOOT/BOOTX64.EFI', 'KERNEL/BROOK.ELF', 'INITRD.IMG', 'BROOK.CFG', 'README.TXT')


def run(argv, **kw):
    print('+', ' '.join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), check=True, **kw)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def fat_sha(image, name, offset=None):
    target = f'{image}@@{offset}' if offset else str(image)
    data = subprocess.check_output(['mtype', '-i', target, '::/' + name])
    return hashlib.sha256(data).hexdigest()


def etc_listing(image):
    out = subprocess.check_output(['mdir', '-a', '-b', '-i', str(image), '::/ETC'], text=True)
    return sorted(line.rstrip('/').rsplit('/', 1)[-1].lower() for line in out.splitlines() if line.startswith('::/'))


def build(args):
    for tool in ('mcopy', 'mmd', 'mtype', 'mdir', 'mkfs.fat', 'sfdisk', 'cmake', 'ninja'):
        if not shutil.which(tool):
            raise SystemExit(f'Missing {tool}; run inside nix-shell')
    if 'HOST_CC' not in os.environ:
        raise SystemExit('Run inside the pinned nix-shell')
    for path in (args.output, args.build_root):
        if os.path.lexists(path):
            raise SystemExit(f'{path} must not exist')
    if args.image is not None and os.path.lexists(args.image):
        raise SystemExit(f'{args.image} must not exist')
    if args.operators.name.endswith('.seed'):
        raise SystemExit('--operators takes the public key file, never a seed')
    policy = args.operators.read_bytes()
    keys = validate_operator_policy(policy)
    if not keys:
        raise SystemExit('At least one operator public key is required')
    commit = subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=no']))
    if dirty and not args.allow_dirty:
        raise SystemExit('Commit tracked changes first (or --allow-dirty for a throwaway build)')

    args.build_root.mkdir(mode=0o700, parents=True)
    operators = args.build_root / 'operators.pub'
    operators.write_bytes(policy)
    kernel_build = args.build_root / 'kernel'
    loader_build = args.build_root / 'loader'
    run(['cmake', '-S', ROOT / 'src/kernel', '-B', kernel_build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/kernel-x86_64-clang.cmake',
         f'-DBROOK_BRIDGE_V3_OPERATOR_KEYS_FILE:FILEPATH={operators}',
         *[f'-D{k}:BOOL={v}' for k, v in KERNEL_FLAGS.items()]])
    run(['cmake', '--build', kernel_build])
    run(['cmake', '-S', ROOT / 'src/bootloader', '-B', loader_build, '-G', 'Ninja',
         f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/uefi-x86_64-clang.cmake',
         *[f'-D{k}:BOOL={v}' for k, v in LOADER_FLAGS.items()]])
    run(['cmake', '--build', loader_build, '--target', 'bootloader'])
    kernel = kernel_build / 'BROOK.elf'
    loader = loader_build / 'BOOTX64.efi'

    # Initrd: only the selected modules, through the standard initrd script.
    stage = args.build_root / 'initrd'
    (stage / 'kernel/drivers').mkdir(parents=True)
    for name in DRIVERS:
        shutil.copy2(kernel_build / 'drivers' / f'{name}.mod', stage / 'kernel/drivers' / f'{name}.mod')
    short = commit[:8] + ('-dirty' if dirty else '')
    payload_id = f'brook-rtx-smoke-{short}'
    env = dict(os.environ, BROOK_BUILD_DIR=str(stage), BROOK_INITRD_MARKERS=','.join(MARKERS),
               BROOK_HARDWARE_PAYLOAD_ID=payload_id)
    env.pop('BROOK_PROVIDER_PAYLOAD', None)
    run(['bash', ROOT / 'scripts/make_initrd.sh', '--release'], env=env)
    initrd = stage / 'esp/INITRD.IMG'
    run(['mmd', '-i', initrd, '::BOOT'])
    init_rc = args.build_root / 'INIT.RC'
    init_rc.write_bytes(INIT_RC)
    run(['mcopy', '-i', initrd, init_rc, '::BOOT/INIT.RC'])
    etc = etc_listing(initrd)
    unexpected = sorted(set(etc) - ETC_ALLOWED)
    if unexpected or not set(MARKERS) <= set(etc):
        raise SystemExit(f'Unexpected initrd /etc contents: {etc}')
    drivers = subprocess.check_output(['mdir', '-b', '-i', str(initrd), '::/DRIVERS'], text=True).split()
    if sorted(d.rsplit('/', 1)[-1].lower() for d in drivers) != sorted(f'{d}.mod' for d in DRIVERS):
        raise SystemExit(f'Unexpected initrd drivers: {drivers}')
    require_keyless_image(initrd)

    staged = args.build_root / 'files'
    sources = {'EFI/BOOT/BOOTX64.EFI': loader, 'KERNEL/BROOK.ELF': kernel, 'INITRD.IMG': initrd}
    for name, src in sources.items():
        (staged / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, staged / name)
    (staged / 'BROOK.CFG').write_bytes(LOADER_CONFIG)
    (staged / 'README.TXT').write_text(README)
    if (staged / 'KERNEL/BROOK.ELF').read_bytes()[:4] != b'\x7fELF' or \
            (staged / 'EFI/BOOT/BOOTX64.EFI').read_bytes()[:2] != b'MZ':
        raise SystemExit('Unexpected kernel or loader format')
    for name in NAMES:
        require_keyless_disk(staged / name)
    files = {name: {'bytes': (staged / name).stat().st_size, 'sha256': sha(staged / name)} for name in NAMES}
    manifest = {
        'format': FORMAT, 'commit': commit, 'dirty': dirty, 'payload_id': payload_id,
        'fat_label': FAT_LABEL, 'files': files,
        'kernel_options': KERNEL_FLAGS, 'loader_options': LOADER_FLAGS,
        'loader_config': LOADER_CONFIG.decode(), 'init_rc': INIT_RC.decode(),
        'initrd': {'drivers': list(DRIVERS), 'etc': etc, 'markers': list(MARKERS)},
        'bridge_v3': {'operator_keys': len(keys), 'policy_sha256': hashlib.sha256(policy).hexdigest()},
        'hardware_scope': {
            'root': 'RAM initrd only; no disk is mounted',
            'storage': 'AHCI unbound (no selectors), no NVMe driver, USB mass storage read-only (EROFS in xhci), '
                       'built-in virtio-blk only exists in VMs',
            'firmware': 'no EFI variable writes, no firmware/flash access, no AML interpreter (uACPI off)',
            'pci_writes': 'every xHCI controller only (BIOS handoff, controller reset, memory+bus-master enable, '
                          'MSI/MSI-X); '
                          'virtio devices exist only in VMs',
            'cpu_msrs': 'volatile only: PAT, APIC, TSC_AUX, EPB 0/15->6 as Linux, MWAIT idle; reset on reboot',
            'inventory': 'read-only config reads (CF8 and MCFG ECAM mapped read-only); no BAR sizing',
            'display': 'firmware GOP framebuffer only',
        },
    }
    staged_manifest = staged / 'manifest.json'
    staged_manifest.write_text(json.dumps(manifest, indent=2) + '\n')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    os.mkdir(args.output, 0o755)
    for item in staged.iterdir():
        item.rename(args.output / item.name)

    if args.image is not None:
        build_image(args.output, files, args.image)
    print(json.dumps(manifest, indent=2))


def build_image(files_dir, files, image):
    """GPT disk with one FAT32 ESP labelled BROOK_RTX, for QEMU (or dd by hand)."""
    mib = 1 << 20
    total = sum(row['bytes'] for row in files.values()) + (files_dir / 'manifest.json').stat().st_size
    esp_bytes = max(64 * mib, ((total * 5 // 4 + 64 * mib + 32 * mib - 1) // (32 * mib)) * (32 * mib))
    image.parent.mkdir(parents=True, exist_ok=True)
    esp = image.with_suffix('.esp.tmp')
    with esp.open('xb') as f:
        f.truncate(esp_bytes)
    run(['mkfs.fat', '-F', '32', '-n', FAT_LABEL, esp])
    run(['mmd', '-i', esp, '::EFI', '::EFI/BOOT', '::KERNEL'])
    for name in [*NAMES, 'manifest.json']:
        run(['mcopy', '-i', esp, files_dir / name, '::/' + name])
        if fat_sha(esp, name) != sha(files_dir / name):
            raise SystemExit('FAT readback mismatch: ' + name)
    with image.open('xb') as f:
        f.truncate(esp_bytes + 2 * mib)
    run(['sfdisk', '--no-reread', '--no-tell-kernel', image],
        input=f'label: gpt\nunit: sectors\n\nstart=2048,size={esp_bytes // 512},type=U,name="BROOK_RTX"\n', text=True)
    with image.open('r+b') as dst, esp.open('rb') as src:
        dst.seek(mib)
        shutil.copyfileobj(src, dst)
    esp.unlink()
    for name in NAMES:
        if fat_sha(image, name, mib) != files[name]['sha256']:
            raise SystemExit('Raw image readback mismatch: ' + name)
    (image.parent / (image.name + '.sha256')).write_text(f'{sha(image)}  {image.name}\n')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True, help='new file-set directory')
    p.add_argument('--build-root', type=Path, required=True, help='new private build directory')
    p.add_argument('--operators', type=Path, required=True, help='operator public key file (one hex key per line)')
    p.add_argument('--image', type=Path, help='also write a raw GPT/FAT32 disk image here')
    p.add_argument('--allow-dirty', action='store_true')
    args = p.parse_args()
    args.output = args.output.absolute()
    args.build_root = args.build_root.absolute()
    args.image = args.image.absolute() if args.image else None
    build(args)


if __name__ == '__main__':
    main()

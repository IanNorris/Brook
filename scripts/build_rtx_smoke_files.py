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
  * Modules: xhci, driving every xHCI controller with USB 2/3 hubs
    (USB storage read-only via /etc/usb-read-only, USB HID boot keyboard and
    mouse, CDC-ECM USB Ethernet), and i219: the onboard Intel I219-V
    (8086:15b8 only). It probes read-only at load; it attaches (rings, link,
    DHCP as a second interface) only with --i219-stage attach|reset or the
    operator's `bridge hardware nic attach|reset`. No GPU or HDA drivers.
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
import re
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from bridge_keyless_image import require_keyless_disk, require_keyless_image  # noqa: E402
from bridge_operator_policy import validate_operator_policy  # noqa: E402

FORMAT = 'brook-rtx-smoke-v3'
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
DRIVERS = ('xhci', 'i219')
I219_STAGES = {'probe': (), 'attach': ('i219-attach',), 'reset': ('i219-reset',)}
MARKERS = ('usb-read-only', 'audio-muted', 'hw-inventory', 'boot-status', 'boot-stick-update')
# Exact /etc contents allowed in the initrd (lower-case FAT names).
ETC_ALLOWED = {*MARKERS, 'i219-attach', 'i219-reset', 'hardware-payload-id', 'resolv.conf', 'machine-id', 'ssl',
               'bridge-v3-operators', 'rtx-update-signer',
               # --session desktop only
               'live-store', 'volatile-desktop', 'hardware-bulk', 'usb-audio-mixer'}
# Desktop live stick (--session desktop): the VMware RC session over the RAM
# store, plus the RTX status helper. If the store does not mount, the services
# fail to start and the kernel console, bridge and remote update stay usable.
DESKTOP_RC = b'''set wm
set vfb none
sched eevdf
run --service --once --console --uid 1000 /nix/profile/bin/hardware-dbus
run --service --once --console --uid 1000 /nix/profile/bin/hardware-message
run --service --once --console /nix/profile/bin/waylandd
run --service --once --console /boot/HWSTATUS.ELF
'''
# Guest helpers in /boot: the boot-stick update receiver and the payload-ID reporter.
HELPERS = {'HWRTXUP.ELF': 'tools/hardware_rtx_update.c', 'HWSTATUS.ELF': 'tools/hardware_status.c'}
INIT_RC = b'set wm\nset vfb none\nkconsole\n'
def loader_config(width=1920, height=1080):
    """BROOK.CFG. DISPLAY_* is the loader's GOP cap: it picks the largest
    firmware mode that fits. BROOK.CFG is not part of PREV/: a cap that broke
    the boot would break the fallback too, so raise it only deliberately."""
    return (b'TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nLOG_MEMORY=0\nLOG_INTERRUPTS=0\n'
            b'UPDATE_SLOTS=0\nRECOVERY_MENU=0\nSTICK_SETS=1\n' +
            f'DISPLAY_WIDTH={width}\nDISPLAY_HEIGHT={height}\n'.encode())


LOADER_CONFIG = loader_config()
README = '''Brook desktop smoke test (RAM only)

Boot once from the firmware boot menu (UEFI entry for this stick).
Secure Boot must be off for this unsigned development loader.
Brook runs from RAM: it does not mount, bind or write internal disks,
does not write EFI variables and does not change firmware settings.
USB storage is read-only, except this stick's own FAT volume during a
signed, operator-authenticated remote update (TCP 1235). PREV/ holds the
previous file set; the loader boots it if a new set was never confirmed.
Remove the stick and reboot to return.
Bridge: v3 operator-authenticated TCP 1234 over a CDC-ECM USB NIC.
'''
NAMES = ('EFI/BOOT/BOOTX64.EFI', 'KERNEL/BROOK.ELF', 'INITRD.IMG', 'BROOK.CFG', 'README.TXT', 'SET.BIN')


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


def stick_set(kernel, initrd, payload_id):
    """SET.BIN (src/shared/update/stick_set.h): the pair the loader may boot."""
    ident = payload_id.encode()
    if not 0 < len(ident) < 48:
        raise SystemExit('payload ID must be 1..47 bytes')
    raw = struct.pack('<8sQQ', b'BRKSET01', len(kernel), len(initrd))
    raw += hashlib.sha256(kernel).digest() + hashlib.sha256(initrd).digest() + ident.ljust(48, b'\0')
    return raw + struct.pack('<I', zlib.crc32(raw))


def build_helpers(build_root, initrd, policy_file, signer_hex):
    """Static musl helpers with the v3 policy digest; the signer public key."""
    vendor = ROOT / 'src/third_party/monocypher'
    objects = []
    for source in (ROOT / 'src/shared/update/bridge_v3.c', vendor / 'monocypher.c', vendor / 'monocypher-ed25519.c'):
        obj = build_root / ('helper-' + source.stem + '.o')
        run([os.environ['MUSL_CC'], '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', vendor, '-c', source, '-o', obj])
        objects.append(obj)
    flags = ['-static', '-O2', '-Wall', '-Wextra', '-Werror', '-DBROOK_BRIDGE_V3',
             f'-DBROOK_BRIDGE_V3_POLICY_SHA256="{sha(policy_file)}"', '-I', vendor]
    run(['mcopy', '-o', '-i', initrd, policy_file, '::/ETC/bridge-v3-operators'])
    signer = build_root / 'rtx-update-signer'
    signer.write_text(signer_hex + '\n')
    run(['mcopy', '-o', '-i', initrd, signer, '::/ETC/rtx-update-signer'])
    digests = {}
    for name, source in HELPERS.items():
        binary = build_root / name
        run([os.environ['MUSL_CC'], *flags, ROOT / source, *objects, '-o', binary])
        run(['mcopy', '-o', '-i', initrd, binary, '::/BOOT/' + name])
        if subprocess.check_output(['mtype', '-i', initrd, '::/BOOT/' + name]) != binary.read_bytes():
            raise SystemExit(name + ' readback mismatch')
        digests[name] = sha(binary)
    return digests


def fat_write(image, path, data):
    subprocess.run(['mcopy', '-o', '-i', str(image), '-', '::/' + path], input=data, check=True)
    if subprocess.check_output(['mtype', '-i', str(image), '::/' + path]) != data:
        raise SystemExit('FAT readback mismatch for ' + path)


def desktop_payload(args, initrd):
    """Desktop session additions (as make_vmware_image.make_initrds), the
    /etc/live-store record, and the EVO4 mixer autostart marker."""
    if not args.live_store:
        raise SystemExit('--session desktop needs --live-store')
    sys.path.insert(0, str(ROOT / 'scripts'))
    from stage_clearwater_shell import add_clearwater_start, embed_clearwater_executables, embed_terminal_shortcut
    from make_vmware_image import wallpaper_raw, check_wallpaper
    store = json.loads(args.live_store.read_text())
    if store.get('format') != 'brook-rtx-live-store/1' or not re.fullmatch(
            r'guid=[0-9a-f]{32} bytes=\d+ sha256=[0-9a-f]{64}\n', store['record']):
        raise SystemExit('unexpected live store manifest')
    profile = Path(store['profile'])
    for directory in ('::/BOOT/BIN', '::/DATA', '::/NIX'):
        if subprocess.run(['mdir', '-i', str(initrd), directory], capture_output=True).returncode:
            run(['mmd', '-i', initrd, directory])
    supervisor = Path(subprocess.check_output(['nix-build', '--no-out-link',
        str(ROOT / 'tools/vulkan-compositor-pkg/clearwater-supervisor.nix')], text=True).strip())
    shell = Path(subprocess.check_output(['nix-build', '--no-out-link',
        str(ROOT / 'tools/clearwater-shell-pkg/default.nix')], text=True).strip())
    embed_clearwater_executables(initrd, supervisor / 'bin/CWSUP.ELF', shell / 'bin/CWSHELL.ELF')
    embed_terminal_shortcut(initrd)
    fat_write(initrd, 'ETC/volatile-desktop', b'1\n')
    fat_write(initrd, 'ETC/hardware-bulk', b'1\n')
    fat_write(initrd, 'ETC/usb-audio-mixer', b'1\n')
    fat_write(initrd, 'ETC/live-store', store['record'].encode())
    fat_write(initrd, 'BOOT/BIN/BASH', (profile / 'bin/bash').read_bytes())
    wallpaper = wallpaper_raw()
    check_wallpaper(wallpaper)
    fat_write(initrd, 'BOOT/WALLPAPER.RAW', wallpaper)
    live = {k: store[k] for k in ('bytes', 'sha256', 'partition_guid', 'partition_name', 'profile')}
    return add_clearwater_start(DESKTOP_RC), live


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
    signer_hex = args.update_signer.read_text().strip()
    if not re.fullmatch('[0-9a-f]{64}', signer_hex):
        raise SystemExit('--update-signer takes a file with the 64-hex-digit Ed25519 public key')
    if args.payload_tag and not re.fullmatch('[a-z0-9]{1,8}', args.payload_tag):
        raise SystemExit('--payload-tag: 1..8 lowercase letters/digits')
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
    payload_id = f'brook-rtx-smoke-{short}' + (f'-{args.payload_tag}' if args.payload_tag else '')
    markers = (*MARKERS, *I219_STAGES[args.i219_stage])
    env = dict(os.environ, BROOK_BUILD_DIR=str(stage), BROOK_INITRD_MARKERS=','.join(markers),
               BROOK_HARDWARE_PAYLOAD_ID=payload_id)
    if args.session == 'desktop':
        env['BROOK_INITRD_KIB'] = '24576'
    env.pop('BROOK_PROVIDER_PAYLOAD', None)
    run(['bash', ROOT / 'scripts/make_initrd.sh', '--release'], env=env)
    initrd = stage / 'esp/INITRD.IMG'
    run(['mmd', '-i', initrd, '::BOOT'])
    init_rc = args.build_root / 'INIT.RC'
    init_rc_bytes = INIT_RC
    live = None
    if args.session == 'desktop':
        init_rc_bytes, live = desktop_payload(args, initrd)
    init_rc.write_bytes(init_rc_bytes)
    run(['mcopy', '-o', '-i', initrd, init_rc, '::BOOT/INIT.RC'])
    helpers = build_helpers(args.build_root, initrd, operators, signer_hex)
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
    config = loader_config(*args.display_max)
    (staged / 'BROOK.CFG').write_bytes(config)
    (staged / 'README.TXT').write_text(README)
    (staged / 'SET.BIN').write_bytes(stick_set((staged / 'KERNEL/BROOK.ELF').read_bytes(),
                                               (staged / 'INITRD.IMG').read_bytes(), payload_id))
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
        'loader_config': config.decode(), 'init_rc': init_rc_bytes.decode(),
        'session': args.session, 'live_store': live,
        'initrd': {'drivers': list(DRIVERS), 'etc': etc, 'markers': list(markers)},
        'i219_stage': args.i219_stage,
        'helpers': helpers,
        'update_signer_public': signer_hex,
        'bridge_v3': {'operator_keys': len(keys), 'policy_sha256': hashlib.sha256(policy).hexdigest()},
        'hardware_scope': {
            'root': 'RAM initrd only; no disk is mounted',
            'storage': 'AHCI unbound (no selectors), no NVMe driver, USB mass storage read-only (EROFS in xhci) '
                       'except a write window over the boot-stick partition during a signed authenticated update, '
                       'built-in virtio-blk only exists in VMs',
            'firmware': 'no EFI variable writes, no firmware/flash access, no AML interpreter (uACPI off)',
            'pci_writes': 'every xHCI controller (BIOS handoff, controller reset, memory+bus-master enable, '
                          'MSI/MSI-X); the I219-V 8086:15b8 command register (memory, bus master, INTx off) '
                          'and its MAC/PHY registers once attached (never NVM/flash cycles); '
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
    p.add_argument('--update-signer', type=Path, required=True,
                   help='file with the boot-stick update Ed25519 public key (hex); see scripts/rtx_smoke_update.py signer-public')
    p.add_argument('--payload-tag', help='suffix for the payload ID (a trivially different set for update tests)')
    p.add_argument('--image', type=Path, help='also write a raw GPT/FAT32 disk image here')
    p.add_argument('--i219-stage', choices=sorted(I219_STAGES), default='probe',
                   help='I219-V bring-up stage at boot (default probe: read-only)')
    p.add_argument('--display-max', default='1920x1080', metavar='WxH',
                   help='GOP mode cap in BROOK.CFG (kernel supports up to 4096x2160); outside PREV/ fallback')
    p.add_argument('--allow-dirty', action='store_true')
    p.add_argument('--session', choices=('kconsole', 'desktop'), default='kconsole',
                   help='desktop: VMware RC desktop over the RAM live store (needs --live-store)')
    p.add_argument('--live-store', type=Path, metavar='STORE_MANIFEST',
                   help='store-manifest.json from build_rtx_live_store.py')
    args = p.parse_args()
    match = re.fullmatch(r'(\d{3,4})x(\d{3,4})', args.display_max)
    if not match or int(match[1]) > 4096 or int(match[2]) > 2160:
        p.error('--display-max: WxH up to 4096x2160')
    args.display_max = (int(match[1]), int(match[2]))
    args.output = args.output.absolute()
    args.build_root = args.build_root.absolute()
    args.image = args.image.absolute() if args.image else None
    build(args)


if __name__ == '__main__':
    main()

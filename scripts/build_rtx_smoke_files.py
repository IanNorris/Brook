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
import uuid
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from bridge_keyless_image import require_keyless_disk, require_keyless_image  # noqa: E402
from bridge_operator_policy import validate_operator_policy  # noqa: E402

FORMAT = 'brook-rtx-smoke-v3'
FAT_LABEL = 'BROOK_RTX'
KERNEL_FLAGS = {
    'BROOK_BRIDGE_V3': 'ON', 'BROOK_NV_DISPLAY': 'ON', 'BROOK_QEMU_LEGACY_OUTBOUND_DEBUG': 'OFF',
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
KERNEL_CONSOLE_SHORTCUT = 'BOOT/SHORTCUTS/KCONSOLE.RC'
DRIVERS = ('xhci', 'i219')
I219_STAGES = {'probe': (), 'attach': ('i219-attach',), 'reset': ('i219-reset',)}
MARKERS = ('usb-read-only', 'audio-muted', 'hw-inventory', 'boot-status', 'boot-stick-update')
# Exact /etc contents allowed in the initrd (lower-case FAT names).
ETC_ALLOWED = {*MARKERS, 'i219-attach', 'i219-reset', 'hardware-payload-id', 'resolv.conf', 'machine-id', 'ssl',
               'keyboard-layout',
               'bridge-v3-operators', 'rtx-update-signer', 'restart-prompt-seconds',
               # --session desktop only
               'live-store', 'usb-system', 'usb-profile-plan.pub', 'usb-system-trim', 'usb-bot-legacy', 'usb-bot-pipeline', 'usb-uas', 'usb-uas-fixture', 'volatile-desktop', 'hardware-bulk', 'usb-audio-mixer', 'dp-idle-seconds', 'nv-display', 'chromium-alsa-device',
               # --nv-gsp (GSP-RM M1 bridge verbs) and its firmware record
               'nv-gsp', 'nv-gsp-firmware', 'nv-gsp-no-autostart', 'nv-drm', 'nv-gsp-registry', 'nv-gsp-no-scrubber', 'usb-system-no-mount', 'nv-gsp-swgen0-ack'}
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


def build_helpers(build_root, initrd, policy_file, signer_hex, *, usb_profiles=False):
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
    helpers = dict(HELPERS)
    if usb_profiles:
        helpers['HWBULK.ELF'] = 'tools/hardware_bulk.c'
    # HWBULKZ.ELF: the general bulk helper (get/put/frame + zlib), so hw_transfer frame/framez
    # screenshots work on the desktop; HWBULK.ELF stays the profile-only receiver.
    helpers['HWBULKZ.ELF'] = 'tools/hardware_bulk.c'
    from build_dell_image import bridge_compression
    zlib_flags, zlib_archive = bridge_compression()
    for name, source in helpers.items():
        binary = build_root / name
        definitions = (['-DBROOK_BULK_USB_PROFILE'] if name == 'HWBULK.ELF' else
                       [*zlib_flags, '-DBROOK_BULK_MARKER="/etc/rtx-update-signer"'] if name == 'HWBULKZ.ELF' else [])
        libraries = [zlib_archive] if name == 'HWBULKZ.ELF' else []
        run([os.environ['MUSL_CC'], *flags, *definitions, ROOT / source, *objects, *libraries, '-o', binary])
        run(['mcopy', '-o', '-i', initrd, binary, '::/BOOT/' + name])
        if subprocess.check_output(['mtype', '-i', initrd, '::/BOOT/' + name]) != binary.read_bytes():
            raise SystemExit(name + ' readback mismatch')
        digests[name] = sha(binary)
    return digests


def fat_write(image, path, data):
    subprocess.run(['mcopy', '-o', '-i', str(image), '-', '::/' + path], input=data, check=True)
    if subprocess.check_output(['mtype', '-i', str(image), '::/' + path]) != data:
        raise SystemExit('FAT readback mismatch for ' + path)


# The GSP-RM firmware set (nix/nv-gsp-firmware.nix output). Every listed file
# must be in its SHA256SUMS; the kernel re-checks its own pinned hashes.
NV_GSP_FIRMWARE_FILES = ('lib/firmware/nvidia/tu104/gsp/gsp-570.144.bin',
                         'lib/firmware/nvidia/tu104/gsp/booter_load-570.144.bin',
                         'lib/firmware/nvidia/tu104/gsp/booter_unload-570.144.bin',
                         'lib/firmware/nvidia/tu104/gsp/bootloader-570.144.bin',
                         'lib/firmware/nvidia/tu104/acr/bl.bin')
NV_GSP_LICENCE_FILES = ('share/licenses/nv-gsp-firmware/LICENCE.nvidia',
                        'share/licenses/nv-gsp-firmware/WHENCE.nouveau')
NV_GSP_INITRD_ROOT = '/boot/nvgsp'


def nv_gsp_firmware_sums(root):
    sums = {}
    for row in (root / 'SHA256SUMS').read_text().splitlines():
        digest, name = row.split('  ', 1)
        sums[name] = digest
    if sorted(sums) != sorted(NV_GSP_FIRMWARE_FILES):
        raise SystemExit(f'{root}: unexpected SHA256SUMS entries')
    for name, digest in sums.items():
        if sha(root / name) != digest:
            raise SystemExit(f'{root}/{name}: sha256 mismatch')
    for name in NV_GSP_LICENCE_FILES:
        if not (root / name).is_file():
            raise SystemExit(f'{root}/{name}: missing licence file')
    return sums


def embed_nv_gsp_firmware(initrd, root):
    """Copy the firmware set and its licence into the initrd under
    /boot/nvgsp (paths as below lib/firmware), read back byte for byte."""
    sums = nv_gsp_firmware_sums(root)
    made = set()
    for name in (*NV_GSP_FIRMWARE_FILES, *NV_GSP_LICENCE_FILES):
        rel = name.removeprefix('lib/firmware/')
        parts = ('BOOT/NVGSP/' + rel).split('/')
        for i in range(1, len(parts)):
            d = '::/' + '/'.join(parts[:i])
            if d not in made and subprocess.run(['mdir', '-i', str(initrd), d], capture_output=True).returncode:
                run(['mmd', '-i', initrd, d])
            made.add(d)
        run(['mcopy', '-o', '-i', initrd, root / name, '::/' + '/'.join(parts)])
        if subprocess.check_output(['mtype', '-i', str(initrd), '::/' + '/'.join(parts)]) != (root / name).read_bytes():
            raise SystemExit('FAT readback mismatch for ' + name)
    fat_write(initrd, 'ETC/nv-gsp-firmware', (NV_GSP_INITRD_ROOT + '\n').encode())
    return {'root': NV_GSP_INITRD_ROOT, 'source': str(root), 'sha256': sums}


def gc_root(path, root):
    """Register an indirect GC root so the host's periodic nix-gc cannot
    delete store paths this set (or its live store) depends on."""
    root.parent.mkdir(parents=True, exist_ok=True)
    run(['nix-store', '--add-root', str(root), '--indirect', '--realise', str(path)], stdout=subprocess.DEVNULL)


def embed_usb_profile_runtime(args, initrd, profile):
    import shlex
    import zipfile

    helper = (profile / 'bin/brook-stage-profile').resolve(strict=True)
    lines = helper.read_text().strip().splitlines()
    if len(lines) != 3 or not lines[1].startswith('export BROOK_STAGE_OPENSSL='):
        raise ValueError('Unexpected selected-profile launcher')
    openssl = lines[1].split('=', 1)[1]
    command = shlex.split(lines[2])
    if len(command) != 7 or command[0] != 'exec' or command[2:5] != ['-B', '-E', '-s'] or command[6] != '$@':
        raise ValueError('Unexpected selected-profile runtime command')
    python, support = command[1], Path(command[5]).parent
    for path in (helper, openssl, python, support):
        if not re.fullmatch(r'/nix/store/[A-Za-z0-9+._/=-]+', str(path)) or '..' in Path(path).parts:
            raise ValueError('Selected-profile runtime is outside the Nix store')
    payload = args.build_root / 'usb-stage.pyz'
    with zipfile.ZipFile(payload, 'x', compression=zipfile.ZIP_STORED) as archive:
        archive.write(ROOT / 'tools/nix-install/stage_usb_profile.py', '__main__.py')
    fat_write(initrd, 'BOOT/USBSTAGE.PYZ', payload.read_bytes())
    launcher = (f'export BROOK_STAGE_OPENSSL={shlex.quote(openssl)}\n'
                f'exec {shlex.quote(python)} -B -E -s /boot/USBSTAGE.PYZ '
                f'{shlex.quote(str(support))} {shlex.quote(str(helper))} {sha(helper)} "$@"\n')
    fat_write(initrd, 'BOOT/USBSTAGE.RC', launcher.encode())


def desktop_payload(args, initrd):
    """Embed desktop boot assets from a signed plan or the fallback RAM store."""
    from stage_clearwater_shell import add_clearwater_start, embed_clearwater_executables, embed_terminal_shortcut
    from make_vmware_image import wallpaper_raw, check_wallpaper
    roots = args.build_root / 'gcroots'
    store = None
    if args.usb_system:
        plan, plan_hash, key_bytes = args.checked_profile_plan
        profile = Path('/nix/store') / plan['profile']['root']
        fat_write(initrd, 'ETC/usb-profile-plan.pub', key_bytes)
        embed_usb_profile_runtime(args, initrd, profile)
    else:
        if not args.live_store:
            raise SystemExit('Fallback desktop requires --live-store')
        store = json.loads(args.live_store.read_text())
        if store.get('format') != 'brook-rtx-live-store/1' or not re.fullmatch(
                r'(guid=[0-9a-f]{32}|lba=\d+ sectors=\d+) bytes=\d+ sha256=[0-9a-f]{64}\n', store['record']):
            raise SystemExit('Unexpected live-store manifest')
        profile = Path(store['profile'])
    if not (profile / 'bin/bash').is_file():
        raise SystemExit(f'Profile {profile} is missing the boot shell. Realise it on the host first.')
    gc_root(profile, roots / 'desktop-profile')
    for directory in ('::/BOOT/BIN', '::/DATA', '::/NIX'):
        if subprocess.run(['mdir', '-i', str(initrd), directory], capture_output=True).returncode:
            run(['mmd', '-i', initrd, directory])
    supervisor = Path(subprocess.check_output(['nix-build', '-o', str(roots / 'clearwater-supervisor'),
        str(ROOT / 'tools/vulkan-compositor-pkg/clearwater-supervisor.nix')], text=True).strip())
    shell = Path(subprocess.check_output(['nix-build', '-o', str(roots / 'clearwater-shell'),
        str(ROOT / 'tools/clearwater-shell-pkg/default.nix')], text=True).strip())
    embed_clearwater_executables(initrd, supervisor / 'bin/CWSUP.ELF', shell / 'bin/CWSHELL.ELF')
    embed_terminal_shortcut(initrd)
    fat_write(initrd, 'ETC/volatile-desktop', b'1\n')
    fat_write(initrd, 'ETC/keyboard-layout', (args.keyboard_layout + '\n').encode())
    fat_write(initrd, 'ETC/hardware-bulk', b'1\n')
    fat_write(initrd, 'ETC/usb-audio-mixer', b'1\n')
    # RTX DP sink idle-off: 5 min, AUX ch 3 / SOR1 link B (the GOP's lit head on this desktop).
    fat_write(initrd, 'ETC/dp-idle-seconds', b'300 3 1 2\n')
    if args.nv_gsp:  # GSP-RM: bridge verbs, and at desktop start p4 keep-display + CE present
        fat_write(initrd, 'ETC/nv-gsp', b'1\n')
        if args.nv_gsp_no_autostart:  # opt out of the boot-time GSP-RM + CE present
            fat_write(initrd, 'ETC/nv-gsp-no-autostart', b'1\n')
        if args.nv_drm:  # M5: /dev/dri/renderD130 (nouveau uAPI for Mesa NVK) once GSP-RM is up
            fat_write(initrd, 'ETC/nv-drm', b'1\n')
        if args.nv_gsp_swgen0_ack:  # A/B: ack GSP SWGEN0 per drain (nouveau r535_gsp_intr)
            fat_write(initrd, 'ETC/nv-gsp-swgen0-ack', b'1\n')
        if args.nv_gsp_no_scrubber:  # A/B: no TU10x GR scrubber channel after golden
            fat_write(initrd, 'ETC/nv-gsp-no-scrubber', b'1\n')
        if args.nv_gsp_registry:  # extra GSP-RM registry DWORDs, "KEY VALUE" lines (A/B tests)
            lines = []
            for kv in args.nv_gsp_registry:
                k, _, v = kv.partition('=')
                if not k or not k.replace('_', '').isalnum() or len(k) > 47:
                    raise SystemExit(f'--nv-gsp-registry: bad key {k!r}')
                int(v, 0)
                lines.append(f'{k} {v}')
            fat_write(initrd, 'ETC/nv-gsp-registry', ('\n'.join(lines) + '\n').encode())
    if args.nv_display:  # desktop presents by flipping head 0 (D2); GOP copy otherwise
        fat_write(initrd, 'ETC/nv-display', b'1\n')
    fat_write(initrd, 'ETC/chromium-alsa-device', b'default\n')  # read by the store's brook-chromium
    if args.usb_system:
        # Persistent /nix on the SSD's BROOK_SYSTEM partition (usb_system.cpp)
        # The volume identity stays fixed across signed profile generations.
        ssd = json.loads(args.usb_system.read_text())
        if ssd.get('format') != 'brook-ssd-system/1' or ssd.get('partition_name') != 'BROOK_SYSTEM' or \
                ssd.get('partition_entry') != 2 or ssd.get('fs_type') != 'ext2':
            raise SystemExit('unexpected ssd-system.json')
        record = (f"BRKUSB1\n{uuid.UUID(ssd['partition_guid']).bytes_le.hex()}\n"
                  f"{int(ssd['first_lba'])},{int(ssd['sectors'])}\n{uuid.UUID(ssd['fs_uuid']).hex}\n")
        fat_write(initrd, 'ETC/usb-system', record.encode())
        if args.usb_uas:
            fat_write(initrd, 'ETC/usb-uas', b'1\n')
            if args.usb_uas_fixture:
                fat_write(initrd, 'ETC/usb-uas-fixture', b'1\n')
        if args.usb_bot != 'default':
            fat_write(initrd, 'ETC/usb-bot-' + args.usb_bot, b'1\n')
        if args.usb_system_no_mount:  # GSP bisect boots: never touch BROOK_SYSTEM (no /nix, no desktop)
            fat_write(initrd, 'ETC/usb-system-no-mount', b'1\n')
        if args.usb_system_trim:
            fat_write(initrd, 'ETC/usb-system-trim', (args.usb_system_trim + '\n').encode())
    else:
        fat_write(initrd, 'ETC/live-store', store['record'].encode())
    fat_write(initrd, 'BOOT/BIN/BASH', (profile / 'bin/bash').read_bytes())
    wallpaper = wallpaper_raw()
    check_wallpaper(wallpaper)
    fat_write(initrd, 'BOOT/WALLPAPER.RAW', wallpaper)
    if args.usb_system:
        storage = {'record': record, 'profile': str(profile), 'plan_sha256': plan_hash,
                   'plan_key_sha256': hashlib.sha256(key_bytes).hexdigest()}
    else:
        storage = {k: store[k] for k in
                   ('bytes', 'sha256', 'partition_guid', 'partition_name', 'profile', 'record', 'slot')
                   if k in store}
    return add_clearwater_start(DESKTOP_RC), storage



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
    if args.usb_system:
        from usb_profile_plan import checked_plan
        args.checked_profile_plan = checked_plan(args.profile_plan, args.profile_plan_public_key)
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

    if args.nv_gsp:
        KERNEL_FLAGS['BROOK_NV_GSP'] = 'ON'
    args.build_root.mkdir(mode=0o700, parents=True)
    operators = args.build_root / 'operators.pub'
    operators.write_bytes(policy)
    kernel_build = args.build_root / 'kernel'
    panic_url = os.environ.get('BROOK_PANIC_INGEST_URL') or \
        (args.panic_ingest_url_file.read_text().strip() if args.panic_ingest_url_file.exists() else '')
    loader_build = args.build_root / 'loader'
    run(['cmake', '-S', ROOT / 'src/kernel', '-B', kernel_build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/kernel-x86_64-clang.cmake',
         f'-DBROOK_BRIDGE_V3_OPERATOR_KEYS_FILE:FILEPATH={operators}',
         f'-DBROOK_PANIC_INGEST_URL:STRING={panic_url}',
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
        # +32 MiB for the 28.7 MB GSP firmware set when it rides in the initrd
        env['BROOK_INITRD_KIB'] = '57344' if args.nv_gsp_firmware else '24576'
    env.pop('BROOK_PROVIDER_PAYLOAD', None)
    run(['bash', ROOT / 'scripts/make_initrd.sh', '--release'], env=env)
    initrd = stage / 'esp/INITRD.IMG'
    run(['mmd', '-i', initrd, '::BOOT'])
    init_rc = args.build_root / 'INIT.RC'
    init_rc_bytes = INIT_RC
    live = None
    shortcuts = {}
    if args.session == 'desktop':
        init_rc_bytes, live = desktop_payload(args, initrd)
        shortcut = (ROOT / 'data/shortcuts/kernel_console.rc').read_bytes()
        fat_write(initrd, KERNEL_CONSOLE_SHORTCUT, shortcut)
        shortcuts[KERNEL_CONSOLE_SHORTCUT] = hashlib.sha256(shortcut).hexdigest()
    if getattr(args, 'restart_prompt_seconds', None) is not None:
        fat_write(initrd, "ETC/restart-prompt-seconds", f"{args.restart_prompt_seconds}\n".encode())
    nv_gsp_fw = embed_nv_gsp_firmware(initrd, args.nv_gsp_firmware) if args.nv_gsp_firmware else None
    if args.nv_gsp_firmware_store:
        # The desktop store already carries the set (build_rtx_live_store.py --nv-gsp-firmware).
        store = json.loads(args.nv_gsp_firmware_store.read_text())
        fw = Path(store['nv_gsp_firmware'])
        nv_gsp_firmware_sums(fw)
        fat_write(initrd, 'ETC/nv-gsp-firmware', f'{fw}/lib/firmware\n'.encode())
        nv_gsp_fw = {'root': f'{fw}/lib/firmware', 'source': 'store', 'store_sha256': store['sha256']}
    init_rc.write_bytes(init_rc_bytes)
    run(['mcopy', '-o', '-i', initrd, init_rc, '::BOOT/INIT.RC'])
    helpers = build_helpers(args.build_root, initrd, operators, signer_hex, usb_profiles=bool(args.usb_system))
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
        'session': args.session, 'live_store': live if not args.usb_system else None,
        'usb_system': live if args.usb_system else None,
        'initrd': {'drivers': list(DRIVERS), 'etc': etc, 'markers': list(markers), 'shortcuts': shortcuts},
        'i219_stage': args.i219_stage,
        'keyboard_layout': args.keyboard_layout,
        'nv_gsp': {'verbs': bool(args.nv_gsp), 'firmware': nv_gsp_fw},
        'helpers': helpers,
        'update_signer_public': signer_hex,
        'bridge_v3': {'operator_keys': len(keys), 'policy_sha256': hashlib.sha256(policy).hexdigest()},
        'hardware_scope': {
            'root': 'RAM initrd; /nix on BROOK_SYSTEM' if args.usb_system else 'RAM initrd',
            'storage': 'AHCI unbound (no selectors), no NVMe driver, USB mass storage read-only (EROFS in xhci) '
                       'except a write window over the boot-stick partition during a signed authenticated update, '
                       'built-in virtio-blk only exists in VMs'
                       + (', persistent writes restricted to BROOK_SYSTEM by its lease' if args.usb_system else ''),
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
    if args.nv_gsp:
        manifest['hardware_scope']['gsp'] = (
            ('bridge calls only (nv-gsp verbs, each GPU-writing verb at most once per boot): '
             if args.nv_gsp_no_autostart else
             'at desktop start GSP-RM boots (p4 keep-display) and the CE present path turns on, plus the '
             'bridge verbs (each GPU-writing verb at most once per boot): ') + 'GPU fn0 PCI command '
            'bus-master enable and GSP/SEC2 falcon registers (FWSEC from the card VBIOS, booter_unload from the '
            'pinned 570.144 set); PROM read-only; no VBIOS/flash writes, no clock/fan/voltage changes')
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
    p.add_argument('--keyboard-layout', choices=('gb', 'us'), default='gb')
    p.add_argument('--restart-prompt-seconds', type=int, choices=range(1, 601), metavar='1..600')
    p.add_argument('--nv-display', action='store_true',
                   help='desktop image: /etc/nv-display = 1 (RTX head-0 flip presentation at desktop start)')
    p.add_argument('--panic-ingest-url-file', type=Path,
                   default=Path('/data/Programming/Brook/shared/dell/panic-ingest-url'),
                   help='file holding the panic scanner URL for the panic QR (same as the Dell build); '
                        'BROOK_PANIC_INGEST_URL overrides; missing file keeps the built-in default')
    p.add_argument('--session', choices=('kconsole', 'desktop'), default='kconsole',
                   help='desktop with a persistent USB system or a fallback live store')
    p.add_argument('--live-store', type=Path, metavar='STORE_MANIFEST',
                   help='store-manifest.json from build_rtx_live_store.py')
    p.add_argument('--usb-system', type=Path, metavar='SSD_SYSTEM_JSON',
                   help='desktop image: boot /nix persistently from the SSD BROOK_SYSTEM partition described by '
                        'write-brook-ssd-system.sh\'s ssd-system.json (writes /etc/usb-system, not /etc/live-store)')
    p.add_argument('--profile-plan', type=Path, help='signed selected profile plan directory for --usb-system')
    p.add_argument('--profile-plan-public-key', type=Path, help='Ed25519 public key admitted by the USB set')
    p.add_argument('--usb-uas', action='store_true', help='try tagged USB Attached SCSI in SuperSpeed')
    p.add_argument('--usb-uas-fixture', action='store_true', help='permit QEMU UAS-only boot fixture discovery')
    p.add_argument('--usb-bot', choices=('default', 'legacy', 'pipeline'), default='default',
                   help='USB system BOT test mode: legacy 64 KiB or experimental read data/status pipelining')
    p.add_argument('--usb-system-trim', metavar="'dry-run|run [MIN_MIB [MAX_MIB]]'",
                   help="scan clean ext2 free extents before mount; send UNMAP only with 'run'")
    p.add_argument('--nv-gsp', action='store_true',
                   help='desktop image: BROOK_NV_GSP kernel and /etc/nv-gsp (GSP-RM verbs; boots GSP-RM + CE present at desktop start unless --nv-gsp-no-autostart)')
    p.add_argument('--usb-system-no-mount', action='store_true',
                   help='with --usb-system: identify the SSD but never mount BROOK_SYSTEM (bisect boots: a forced '
                        'hang cannot dirty the store; boot updates still allowed; desktop and lastlog stay off)')
    p.add_argument('--nv-gsp-swgen0-ack', action='store_true', help='with --nv-gsp: ack GSP SWGEN0 each drain (A/B)')
    p.add_argument('--nv-gsp-no-scrubber', action='store_true', help='with --nv-gsp: skip the TU10x GR scrubber channel (A/B)')
    p.add_argument('--nv-gsp-registry', action='append', default=[], metavar='KEY=VALUE',
                   help='with --nv-gsp: extra GSP-RM registry DWORD (repeatable), e.g. RMElpg=0')
    p.add_argument('--nv-drm', action='store_true',
                   help='with --nv-gsp: /etc/nv-drm (M5: /dev/dri/renderD130, the nouveau uAPI subset for Mesa NVK, '
                        'published once GSP-RM is up)')
    p.add_argument('--nv-gsp-no-autostart', action='store_true',
                   help='with --nv-gsp: /etc/nv-gsp-no-autostart (no GSP-RM / CE present at boot; verbs only)')
    p.add_argument('--nv-gsp-firmware', type=Path, metavar='NIX_OUT',
                   help='nix/nv-gsp-firmware.nix output to carry in the signed initrd (/boot/nvgsp, /etc/nv-gsp-firmware)')
    p.add_argument('--nv-gsp-firmware-store', type=Path, metavar='STORE_MANIFEST',
                   help='store-manifest.json of a store built with --nv-gsp-firmware: /etc/nv-gsp-firmware names '
                        'the store copy (the store itself must be the one this set boots)')
    args = p.parse_args()
    if args.nv_gsp_firmware and args.nv_gsp_firmware_store:
        p.error('--nv-gsp-firmware and --nv-gsp-firmware-store are alternatives')
    if args.nv_gsp_firmware_store and not args.nv_gsp:
        p.error('--nv-gsp-firmware-store needs --nv-gsp')
    if args.nv_gsp_firmware and not (args.nv_gsp and args.session == 'desktop'):
        p.error('--nv-gsp-firmware needs --nv-gsp and --session desktop')
    if args.nv_gsp and args.session != 'desktop':
        p.error('--nv-gsp needs --session desktop')
    if args.usb_system:
        if args.session != 'desktop' or not args.profile_plan or not args.profile_plan_public_key or args.live_store:
            p.error('--usb-system needs --session desktop, --profile-plan and --profile-plan-public-key')
    elif args.profile_plan or args.profile_plan_public_key:
        p.error('Profile plans require --usb-system')
    if args.usb_uas_fixture and not args.usb_uas:
        p.error("--usb-uas-fixture needs --usb-uas")
    if args.usb_uas and not args.usb_system:
        p.error("--usb-uas needs --usb-system")
    if args.usb_bot != "default" and not args.usb_system:
        p.error("--usb-bot needs --usb-system")
    if args.usb_system_trim and (not args.usb_system or not re.fullmatch(
            r'(dry-run|run)( [0-9]+){0,2}', args.usb_system_trim)):
        p.error("--usb-system-trim needs --usb-system and 'dry-run' or 'run' with optional MiB limits")

    match = re.fullmatch(r'(\d{3,4})x(\d{3,4})', args.display_max)
    if not match or int(match[1]) > 4096 or int(match[2]) > 2160:
        p.error('--display-max: WxH up to 4096x2160')
    args.display_max = (int(match[1]), int(match[2]))
    args.output = args.output.absolute()
    args.build_root = args.build_root.absolute()
    args.image = args.image.absolute() if args.image else None
    args.nv_gsp_firmware = args.nv_gsp_firmware.resolve() if args.nv_gsp_firmware else None
    if args.usb_system_trim and args.usb_system_trim.split()[0] == "run":
        fields = args.usb_system_trim.split()
        if len(fields) != 3 or not 0 < int(fields[1]) <= int(fields[2]) <= 524288:
            p.error("--usb-system-trim run requires positive MIN_MIB and MAX_MIB")
    build(args)


if __name__ == '__main__':
    main()

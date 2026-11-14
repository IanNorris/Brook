#!/usr/bin/env python3
"""Build a Dell XPS 13 9343 boot pair (kernel.elf + INITRD.IMG) reproducibly from this checkout.

Run inside the pinned dev shell from the repository root:

    nix-shell --run 'python3 scripts/build_dell_image.py --payload-id dell-main-20261114 --output build/dell/pair'

Steps (each is deterministic for a given checkout and asset set):
  1. Configure and build the guarded Dell kernel in build/dell/kernel with a fixed option set.
  2. nix-build the userspace boot services: the Vulkan desktop supervisor and renderer, and the
     Vulkan-enabled waylandd ("INTLWL").
  3. prepare_hardware_kernel_pair.py embeds them, plus the matched modules, into the pinned source initrd.
  4. verify_guarded_desktop_candidate.py checks the result independently.
  5. build-manifest.json records the commit, options, store paths, hashes and the runtime store
     paths the boot binaries need on the target.
  6. With --qemu-check, scripts/check_dell_image_qemu.py runs the guarded recovery-to-desktop
     QEMU/KVM test on the exact pair (the pre-deployment gate).

This prepares private files only. Deployment is separate (scripts/hardware_update_device.py) and needs
the target to already contain the runtime store paths listed in the manifest.
"""
import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SHARED = Path('/data/Programming/Brook/shared/dell')
SOURCE_INITRD = 'source-initrd-cc876d82.img'
SOURCE_INITRD_SHA = 'cc876d82a1f523f6b60bacabba883daf3a4482ab012e80580fabfa9bafdb6a11'
MODULE_ABI_HEADER = ROOT / 'src/kernel/src/module_abi.h'

# Kernel options matching the accepted gen 338/355 Dell baseline.
KERNEL_OPTIONS = {
    'BROOK_SSD_GUARD_CHECK': 'ON', 'BROOK_SSD_AUTO_RECOVERY': 'ON',
    'BROOK_SSD_GUARDED_DESKTOP': 'ON', 'BROOK_SSD_GUARDED_DESKTOP_RESET': 'ON',
    'BROOK_DELL_ACPI_MONITOR': 'ON', 'BROOK_BDW_COHERENT_RENDER': 'ON',
    'BROOK_BDW_RPS_TRIAL': 'ON', 'BROOK_GPU_SUBMISSION_TIMING': 'ON',
}
# Options accepted by --kernel-option, mapped to the verifier flag each needs (None = none needed).
VERIFY_ALLOW = {
    'BROOK_GPU_SUBMISSION_TIMING': '--allow-gpu-submission-timing',
    'BROOK_BDW_COHERENT_RENDER': '--allow-bdw-coherent-render',
    'BROOK_BDW_RPS_TRIAL': '--allow-bdw-rps-trial',
    'BROOK_DELL_ACPI_MONITOR': '--allow-dell-acpi-monitor',
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(argv, **kw):
    print('+', ' '.join(str(a) for a in argv), flush=True)
    return subprocess.run([str(a) for a in argv], check=True, **kw)


def nix_build(expr_path, *args):
    out = subprocess.run(['nix-build', str(expr_path), '--no-out-link', *args], check=True,
                         capture_output=True, text=True).stdout.strip().splitlines()[-1]
    return Path(out)


def runtime_paths(elf):
    """Store paths an embedded dynamic ELF needs on the target: interpreter and RUNPATH."""
    text = subprocess.run(['readelf', '-d', '-p', '.interp', str(elf)], capture_output=True, text=True).stdout
    paths = set()
    for line in text.splitlines():
        if 'RUNPATH' in line or 'RPATH' in line:
            for p in line.split('[', 1)[1].rstrip(']').split(':'):
                paths.add('/'.join(p.split('/')[:4]))
        elif '/nix/store/' in line:
            p = line[line.index('/nix/store/'):].strip()
            paths.add('/'.join(p.split('/')[:4]))
    return sorted(paths)


def module_abi():
    for line in MODULE_ABI_HEADER.read_text().splitlines():
        if 'MODULE_ABI_VERSION =' in line:
            return int(line.split('=')[1].strip().rstrip(';'))
    raise RuntimeError('MODULE_ABI_VERSION not found')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--payload-id', required=True)
    p.add_argument('--output', type=Path, required=True, help='new pair directory (must not exist)')
    p.add_argument('--shared', type=Path, default=DEFAULT_SHARED, help='private Dell asset directory')
    p.add_argument('--kernel-build', type=Path, default=ROOT / 'build/dell/kernel')
    p.add_argument('--kernel-option', action='append', default=[], metavar='NAME=ON|OFF',
                   help='override one kernel option (repeatable)')
    p.add_argument('--supervisor', default='brook-desktop-supervisor-serial',
                   help='supervisor variant from the vulkan-compositor package')
    p.add_argument('--allow-dirty', action='store_true', help='permit uncommitted changes (recorded)')
    p.add_argument('--qemu-check', action='store_true',
                   help='then run scripts/check_dell_image_qemu.py on the pair (results in <output>/qemu-check)')
    a = p.parse_args()
    a.output, a.shared = a.output.resolve(), a.shared.resolve()

    if 'HOST_CC' not in os.environ:
        sys.exit('Run inside the pinned dev shell: nix-shell --run "python3 scripts/build_dell_image.py ..."')
    if a.output.exists():
        sys.exit(f'{a.output} already exists; pairs are never overwritten')
    commit = subprocess.run(['git', '-C', ROOT, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(['git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=no'],
                                capture_output=True, text=True, check=True).stdout.strip())
    if dirty and not a.allow_dirty:
        sys.exit('Working tree has uncommitted changes; commit them or pass --allow-dirty')

    source = a.shared / SOURCE_INITRD
    if sha(source) != SOURCE_INITRD_SHA:
        sys.exit(f'{source} does not match the pinned source initrd')

    options = dict(KERNEL_OPTIONS)
    for item in a.kernel_option:
        name, _, value = item.partition('=')
        if value not in ('ON', 'OFF') or not name.startswith('BROOK_'):
            sys.exit(f'bad --kernel-option {item}')
        options[name] = value

    # 1. Kernel (fresh worktrees have no submodules; only these two are needed)
    if not (ROOT / 'src/third_party/fatfs/source/ff.c').exists() or \
       not (ROOT / 'vendor/uefi-headers/Include/Uefi.h').exists():
        run(['git', '-C', ROOT, 'submodule', 'update', '--init', '--recursive', '--',
             'vendor/uefi-headers', 'src/third_party/fatfs'])
    build = a.kernel_build.resolve()
    configure = ['cmake', '-S', ROOT / 'src/kernel', '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                 f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/kernel-x86_64-clang.cmake']
    configure += [f'-D{k}:BOOL={v}' for k, v in sorted(options.items())]
    archive = a.shared / 'uacpi-6.1.0.tar.gz'
    if archive.exists():
        configure.append(f'-DBROOK_UACPI_ARCHIVE={archive}')
    run(configure)
    run(['cmake', '--build', build])
    kernel = build / 'BROOK.elf'

    # 2. Userspace boot services
    compositor = nix_build(ROOT / 'tools/vulkan-compositor-pkg')
    waylandd = nix_build(ROOT / 'tools/waylandd-pkg', '--arg', 'enableVulkanCompositor', 'true')
    supervisor = compositor / 'bin' / a.supervisor
    renderer = compositor / 'bin/brook-desktop-renderer'
    provider = waylandd / 'bin/waylandd'

    # 3. Prepare the pair
    abi = module_abi()
    run([sys.executable, ROOT / 'scripts/prepare_hardware_kernel_pair.py', '--source-initrd', source,
         '--build', build, '--abi', abi, '--payload-id', a.payload_id, '--output', a.output,
         '--expected-kernel-sha256', sha(kernel), '--enable-efi-poweroff', '--enable-usb-boot-poweroff',
         '--enable-dell-acpi-monitor', '--enable-ac-loss-poweroff', '--enable-vulkan-desktop',
         '--enable-vulkan-async-partial', '--enable-dell-bcs-present', '--vulkan-supervisor', supervisor,
         '--vulkan-renderer', renderer, '--gpu-source-provider', provider], cwd=ROOT / 'scripts')

    # 4. Independent verification
    verify = [sys.executable, ROOT / 'scripts/verify_guarded_desktop_candidate.py', '--candidate', a.output,
              '--source', source, '--reference', source, '--build', build, '--expected-kernel', sha(kernel),
              '--expected-source', SOURCE_INITRD_SHA, '--expected-reference', SOURCE_INITRD_SHA, '--abi', abi,
              '--allow-vulkan-desktop', '--allow-vulkan-async-partial', '--allow-dell-bcs-present',
              '--allow-ac-loss-poweroff', '--expected-vulkan-supervisor', sha(supervisor),
              '--expected-vulkan-renderer', sha(renderer), '--expected-gpu-provider', sha(provider)]
    verify += [flag for name, flag in VERIFY_ALLOW.items() if options.get(name) == 'ON']
    result = subprocess.run([str(x) for x in verify], cwd=ROOT / 'scripts', capture_output=True, text=True)
    if result.returncode:
        sys.exit('verification failed:\n' + result.stdout[-2000:] + result.stderr[-4000:])
    (a.output / 'verify.json').write_text(result.stdout)

    # 5. Manifest
    manifest = {
        'payload_id': a.payload_id, 'commit': commit, 'dirty': dirty, 'module_abi': abi,
        'kernel_options': options, 'kernel_sha256': sha(a.output / 'kernel.elf'),
        'initrd_sha256': sha(a.output / 'INITRD.IMG'), 'source_initrd_sha256': SOURCE_INITRD_SHA,
        'packages': {'vulkan_compositor': str(compositor), 'waylandd_vulkan': str(waylandd)},
        'boot_services': {name: {'source': str(path), 'sha256': sha(path), 'runtime_store_paths': runtime_paths(path)}
                          for name, path in (('VKSUP.ELF', supervisor), ('VKRENDER.ELF', renderer), ('INTLWL.ELF', provider))},
    }
    (a.output / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({k: manifest[k] for k in ('payload_id', 'commit', 'dirty', 'kernel_sha256', 'initrd_sha256')}, indent=2))

    # 6. Optional guarded recovery-to-desktop QEMU/KVM check of the exact pair
    if a.qemu_check:
        check = subprocess.run([sys.executable, ROOT / 'scripts/check_dell_image_qemu.py', '--pair', a.output,
                                '--kernel-build', build, '--shared', a.shared])
        if check.returncode:
            sys.exit(f'QEMU check failed; see {a.output}/qemu-check/verdict.json')


if __name__ == '__main__':
    main()

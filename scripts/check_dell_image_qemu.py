#!/usr/bin/env python3
"""Guarded recovery-to-desktop QEMU/KVM check for a Dell boot pair.

Run inside the pinned dev shell from the repository root:

    nix-shell --run 'python3 scripts/check_dell_image_qemu.py --pair build/dell/pair-test'

This is the pre-deployment gate the gen 338 baseline ("short-lock-serial") passed. It boots the
exact pair's kernel and initrd (Dell startup script included) on a disposable 128 MiB QEMU SATA
disk whose ext2 data volume starts dirty: firmware must select automatic recovery, which repairs
and verifies the volume, publishes clean guard records and resets; the next boot must select the
pair, pass the guard check and run the Dell startup script to GUARD_DESKTOP_HANDOFF_SCRIPT status=0.
The pass marker is GUARD_REPAIR_TO_DESKTOP_PASS.

Steps:
  1. Build a QEMU-only desktop pair in a scratch directory: the pair's kernel.elf and INITRD.IMG
     byte for byte, a SELECT.BIN for them, and a qemu-fixture manifest bound to the pinned recovery
     fixture's identity, with kernel_build_flags read from the kernel build's CMakeCache.txt (or from
     --kernel-build-flags JSON when the build directory no longer exists). This is how every earlier
     Dell QEMU pair was made; prepare_guard_check_pair.py cannot produce it because it builds a fresh
     recovery initrd and rejects guarded-desktop kernels.
  2. Run scripts/test_guard_repair_handoff_qemu.py with the gen 338 mode flags:
         --auto-desktop --auto-desktop-dell-initrd --auto-desktop-intel-provider
         --auto-desktop-vulkan-service
     (reconstructed from qemu-short-lock-serial: dell-startup-script.sha256 present, and the
     desktop log shows /boot/INTLWL.ELF and /boot/VKSUP.ELF with the synthetic service's hash).
  3. Write <pair>/qemu-check/: test-N.log and the test's serial logs under run-N/ for each attempt,
     the scratch pair's manifest and verdict.json. Exit status is nonzero on failure. Only an OVMF
     stall before boot selection (no "BdsDxe" line before the 120 s timeout, seen once) is retried.

Pinned inputs (in /data/Programming/Brook/shared/dell, listed in its SHA256SUMS; the SHA-256 values
below are of the decompressed files and are checked before use):
  qemu-check/guard-efi-fixture-20261103.raw.gz   disposable GPT disk, identity BROOK-RECOVERY-QA
      (legacy build/hardware/guard-efi-fixture-20261103.raw)                     aeffc80c...
  qemu-check/BROOKGUARD-qemu-v5.efi   QEMU-identity guard loader
      (legacy build/hardware/guard-efi-v5-qemu-20261107/BROOKGUARD.efi)          bfae0787...
  qemu-check/recovery-v2/   automatic recovery pair (KERNEL.ELF a7ffc35c..., INITRD.IMG.gz ->
      6a8bfced..., SELECT.BIN, manifest.json; legacy build/hardware/guard-recovery-v2-qemu-fixture-20261105)
  rollback-gen355/kernel-build-flags.json   build flags recorded for the gen 338/355 kernel, whose
      build directory no longer exists (use with --kernel-build-flags)
  Firmware: the dev shell's OVMF ($OVMF_CODE directory), OVMF_CODE.fd SHA-256 recorded in the verdict.

What it does not exercise: the Intel GPU, Vulkan supervisor, renderer and Wayland provider (all
replaced by a synthetic static service, so the real boot-service binaries are never run), the Dell
USB network, real ACPI/EC, firmware reset behaviour, the physical SSD and the physical-identity loader.
It checks the guarded boot, recovery, reset and startup-script path for this kernel and initrd.
With --bridge-v3-qa, the guarded desktop also runs authenticated commands and
the packaged bulk helpers on loopback-only QEMU forwards, using a synthetic operator.
"""
import argparse, gzip, hashlib, json, os, re, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_update_protocol import record
from guarded_bridge_v3_qa import PASS_MARKER as BRIDGE_V3_PASS_MARKER

DEFAULT_SHARED = Path('/data/Programming/Brook/shared/dell')
IMAGE = ('qemu-check/guard-efi-fixture-20261103.raw.gz',
         'aeffc80cb3757375f3e7233457767650ecf09742cd35133e8c75f56b480fe541')
EFI = ('qemu-check/BROOKGUARD-qemu-v5.efi', 'bfae0787c366968fef46ca388d935114d7d815c37be63e8d12970b739529ffde')
RECOVERY = {
    'KERNEL.ELF': 'a7ffc35c5ea8615cf08c73bec91dd606d55b097abe8b0708e7737c7fd519eb22',
    'INITRD.IMG': '6a8bfcede31fac2b90791ffb0aeef4304ccbafd55ee7e26e977888358555b055',
    'SELECT.BIN': 'ba4870d6209fc18b9500134e9c6cab3830523f5ca6cc285d5209a8b3d3da81ef',
    'manifest.json': '94851ccfbe48d6be9b205a9e94460a227e6d8466c9a31f427b96eebf80e7e923',
}
MODE_FLAGS = ['--auto-desktop', '--auto-desktop-dell-initrd', '--auto-desktop-intel-provider',
              '--auto-desktop-vulkan-service']
PASS_MARKER = 'GUARD_REPAIR_TO_DESKTOP_PASS'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pinned(src, dest, digest):
    """Copy (decompressing .gz) a pinned input to dest and check its digest."""
    if src.suffix == '.gz':
        with gzip.open(src, 'rb') as f, dest.open('wb') as out:
            shutil.copyfileobj(f, out)
    else:
        shutil.copyfile(src, dest)
    if sha(dest) != digest:
        raise SystemExit(f'pinned input {src} does not match {digest}')


def build_flags(a, kernel_sha):
    if a.kernel_build_flags:
        data = json.loads(a.kernel_build_flags.read_text())
        if data.get('kernel_sha256') not in (None, kernel_sha):
            raise SystemExit(f'{a.kernel_build_flags} describes kernel {data["kernel_sha256"]}, not {kernel_sha}')
        return data['kernel_build_flags'], data.get('build_cache_sha256'), str(a.kernel_build_flags)
    build = a.kernel_build.resolve()
    if sha(build / 'BROOK.elf') != kernel_sha:
        raise SystemExit(f'{build}/BROOK.elf is not the pair kernel; pass the matching --kernel-build '
                         'or --kernel-build-flags')
    cache = (build / 'CMakeCache.txt').read_text()
    flags = dict(re.findall(r'^(BROOK_[A-Z0-9_]+):BOOL=(ON|OFF)$', cache, re.M))
    return flags, hashlib.sha256(cache.encode()).hexdigest(), str(build)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--pair', type=Path, required=True, help='pair directory (kernel.elf, INITRD.IMG)')
    p.add_argument('--kernel-build', type=Path, default=ROOT / 'build/dell/kernel',
                   help='kernel build directory whose BROOK.elf is the pair kernel')
    p.add_argument('--kernel-build-flags', type=Path,
                   help='JSON with kernel_build_flags, for a pair whose build directory is gone')
    p.add_argument('--shared', type=Path, default=DEFAULT_SHARED)
    p.add_argument('--efi', type=Path,
                   help='QEMU-identity guarded loader to test instead of the pinned v5 one (e.g. a '
                        'build_dell_loader.py build with --shared <dir holding the QEMU fixture options>)')
    p.add_argument('--firmware', type=Path, help='OVMF FV directory (default: dirname of $OVMF_CODE)')
    p.add_argument('--output', type=Path, help='result directory (default: <pair>/qemu-check)')
    p.add_argument('--force', action='store_true', help='replace an existing result directory')
    p.add_argument('--attempts', type=int, default=2,
                   help='maximum runs; only a firmware stall before boot selection is retried (default 2)')
    p.add_argument('--proc-dump-qa', action='store_true', help='Also run the two-thread live core and GDB fixture')
    p.add_argument('--proc-autodump-qa', action='store_true', help='Run the live core fixture and verify an automatic fault core')
    p.add_argument('--managed-profile', action='store_true',
                   help='Seed the data volume with the signed-staging profile layout (aliases into a generation)')
    p.add_argument('--managed-profile-absolute-current', action='store_true',
                   help='Negative check: with --managed-profile, an absolute current alias must fail handoff')
    p.add_argument('--bridge-local-policy-qa', type=Path,
                   help='Run the local bridge and reserved-port guest regression using this static probe')
    p.add_argument('--bridge-v3-qa', type=Path, metavar='OPERATOR.SEED',
                   help='Test the exact v3 pair with its synthetic operator seed and matching virtio module')
    a = p.parse_args()
    mode_flags = list(MODE_FLAGS)
    if a.managed_profile:
        mode_flags.append('--managed-profile')
    if a.managed_profile_absolute_current:
        if not a.managed_profile:
            p.error('--managed-profile-absolute-current requires --managed-profile')
        mode_flags.append('--managed-profile-absolute-current')
    if a.proc_dump_qa or a.proc_autodump_qa:
        mode_flags += ['--proc-dump-qa', str(a.kernel_build.resolve() / 'drivers/virtio_net.mod')]
    if a.proc_autodump_qa:
        mode_flags.append('--proc-autodump-qa')
    if a.bridge_local_policy_qa:
        if a.proc_dump_qa or a.proc_autodump_qa or a.managed_profile_absolute_current:
            p.error('the bridge local policy probe requires an ordinary guarded handoff')
        mode_flags += ['--bridge-local-policy-qa', str(a.bridge_local_policy_qa.resolve()),
                       '--auto-desktop-network-module', str(a.kernel_build.resolve() / 'drivers/virtio_net.mod')]
    if a.bridge_v3_qa:
        if (a.bridge_local_policy_qa or a.proc_dump_qa or a.proc_autodump_qa or
                a.managed_profile_absolute_current or a.kernel_build_flags):
            p.error('v3 QA requires the ordinary guarded handoff and the matching kernel build directory')
        mode_flags += ['--bridge-v3-qa', str(a.bridge_v3_qa.absolute()),
                       '--auto-desktop-network-module', str(a.kernel_build.resolve() / 'drivers/virtio_net.mod')]
    pair, shared = a.pair.resolve(), a.shared.resolve()
    out = (a.output or pair / 'qemu-check').resolve()
    if 'OVMF_CODE' not in os.environ and not a.firmware:
        sys.exit('Run inside the pinned dev shell: nix-shell --run "python3 scripts/check_dell_image_qemu.py ..."')
    firmware = (a.firmware or Path(os.environ['OVMF_CODE']).parent).resolve()
    if out.exists():
        if not a.force:
            sys.exit(f'{out} exists; pass --force to replace it')
        shutil.rmtree(out)
    out.mkdir(parents=True)

    manifest_path = pair / 'manifest.json'
    expect_clearwater = (manifest_path.exists() and
                         bool(json.loads(manifest_path.read_text()).get('clearwater_executables_sha256')))
    kernel, initrd = pair / 'kernel.elf', pair / 'INITRD.IMG'
    kernel_sha, initrd_sha = sha(kernel), sha(initrd)
    flags, cache_sha, flag_source = build_flags(a, kernel_sha)
    if a.bridge_v3_qa:
        if flags.get('BROOK_BRIDGE_V3') != 'ON':
            p.error('v3 QA requires a v3 kernel')
        build_manifest = json.loads((pair / 'build-manifest.json').read_text())
        policy = build_manifest.get('bridge_v3') or {}
        if (build_manifest.get('dirty') is not False or
                build_manifest.get('kernel_sha256') != kernel_sha or
                build_manifest.get('initrd_sha256') != initrd_sha or
                (policy.get('version'), policy.get('suite')) != (3, 1)):
            p.error('v3 QA requires a clean exact v3 build manifest')
        bridge_manifest = {
            'build_id': build_manifest['build_id'],
            'kernel_sha256': kernel_sha,
            'source_initrd_sha256': initrd_sha,
            'policy_sha256': policy['policy_sha256'],
            'helpers_sha256': policy['helpers_sha256'],
        }

    # Short scratch path: QEMU monitor sockets below the test's temporary directory must fit in
    # sun_path (108 bytes).
    tmp_root = ROOT / 'build/qc'
    if len(str(tmp_root)) > 60:
        raise SystemExit(f'{tmp_root} is too long for QEMU socket paths')
    scratch = tmp_root / f'p{os.getpid()}'
    scratch.mkdir(parents=True)
    verdict = {'pair': str(pair), 'kernel_sha256': kernel_sha, 'initrd_sha256': initrd_sha,
               'kernel_build_flags_source': flag_source, 'kernel_build_flags': flags, 'mode_flags': mode_flags,
               'pinned_inputs': {'image': IMAGE[1], 'efi': sha(a.efi) if a.efi else EFI[1], 'recovery_pair': RECOVERY,
                                 'ovmf_code_sha256': sha(firmware / 'OVMF_CODE.fd'), 'ovmf_dir': str(firmware)}}
    try:
        image = scratch / 'fixture.raw'
        pinned(shared / IMAGE[0], image, IMAGE[1])
        efi = scratch / 'BROOKGUARD.efi'
        if a.efi:
            shutil.copyfile(a.efi, efi)  # caller-supplied loader; its hash is in the verdict
        else:
            pinned(shared / EFI[0], efi, EFI[1])
        recovery = scratch / 'recovery-pair'
        recovery.mkdir()
        for name, digest in RECOVERY.items():
            src = shared / 'qemu-check/recovery-v2' / name
            pinned(src.with_name(name + '.gz') if not src.exists() else src, recovery / name, digest)
        rmanifest = json.loads((recovery / 'manifest.json').read_text())

        # 1. QEMU-only desktop pair: exact kernel and initrd, fixture identity.
        desktop = scratch / 'desktop-pair'
        desktop.mkdir()
        shutil.copyfile(kernel, desktop / 'KERNEL.ELF')
        shutil.copyfile(initrd, desktop / 'INITRD.IMG')
        selection = record(1, 0, kernel.read_bytes(), initrd.read_bytes())
        (desktop / 'SELECT.BIN').write_bytes(selection)
        manifest = {
            'scope': 'Disposable QEMU recovery fixture for Dell pair check; never deploy',
            'profile': 'qemu-fixture', 'input_sha256': rmanifest['input_sha256'],
            'build_cache_sha256': cache_sha, 'kernel_build_flags': flags,
            'checker_sha256': rmanifest['checker_sha256'], 'guard_identity': rmanifest['guard_identity'],
            'data_first': rmanifest['data_first'], 'data_sectors': rmanifest['data_sectors'],
            'files': {n: {'bytes': (desktop / n).stat().st_size, 'sha256': sha(desktop / n)}
                      for n in ('KERNEL.ELF', 'INITRD.IMG', 'SELECT.BIN')},
            'ext2_checker_sha256': rmanifest['ext2_checker_sha256'],
        }
        if manifest['files']['KERNEL.ELF']['sha256'] != kernel_sha or manifest['files']['INITRD.IMG']['sha256'] != initrd_sha:
            raise SystemExit('pair changed while copying')
        if a.bridge_v3_qa:
            manifest['bridge_v3_qa'] = bridge_manifest
        text = json.dumps(manifest, indent=2) + '\n'
        (desktop / 'manifest.json').write_text(text)
        (out / 'qemu-pair-manifest.json').write_text(text)

        # 2. The guarded repair-to-desktop test.
        env = dict(os.environ, TMPDIR=str(tmp_root))  # the test's disposable disk lives here
        verdict['attempts'] = []
        for attempt in range(1, a.attempts + 1):
            run_dir, log_path = out / f'run-{attempt}', out / f'test-{attempt}.log'
            argv = [sys.executable, str(ROOT / 'scripts/test_guard_repair_handoff_qemu.py'),
                    '--image', str(image), '--efi', str(efi), '--firmware', str(firmware),
                    '--desktop-pair', str(desktop), '--recovery-pair', str(recovery),
                    '--output', str(run_dir), '--identity', rmanifest['guard_identity'], *mode_flags]
            print('+', ' '.join(argv), flush=True)
            start = time.monotonic()
            with log_path.open('w') as log:
                result = subprocess.run(argv, cwd=ROOT / 'scripts', env=env, stdout=log, stderr=subprocess.STDOUT)
            lines = log_path.read_text(errors='replace').splitlines()
            passed = result.returncode == 0 and PASS_MARKER in lines
            bridge_checked = a.bridge_v3_qa and BRIDGE_V3_PASS_MARKER in lines and (run_dir / 'bridge-v3-qa.json').is_file()
            if a.bridge_v3_qa and passed and not bridge_checked:
                lines.append('Guarded v3 boot did not verify all four packaged receiver protocols')
                passed = False
            clearwater_ready = False
            if expect_clearwater and passed:
                serial = (run_dir / 'desktop-serial.log').read_text(errors='replace')
                clearwater_ready = bool(re.search(r'CLEARWATER_SHELL_READY pid=[1-9][0-9]*', serial))
                if not clearwater_ready:
                    lines.append('Clearwater shell did not publish a taskbar in the guarded desktop boot')
                    passed = False
            last = lines[-1] if lines else ''
            # OVMF occasionally stalls before reaching boot selection (seen once: the serial log held
            # only the firmware's screen-clear sequences for the whole 120 s). Only that signature,
            # which says nothing about the pair, is retried.
            firmware_stall = (not passed and last.startswith('RuntimeError: timed out waiting for')
                              and 'BdsDxe' not in last)
            entry = {'attempt': attempt, 'command': argv, 'returncode': result.returncode, 'passed': passed,
                     'seconds': round(time.monotonic() - start, 1), 'log': log_path.name, 'output': run_dir.name}
            if expect_clearwater:
                entry['clearwater_shell_ready'] = clearwater_ready
            if bridge_checked:
                entry['bridge_v3_evidence'] = 'bridge-v3-qa.json'
                entry['bridge_v3_evidence_sha256'] = sha(run_dir / 'bridge-v3-qa.json')
            if not passed:
                entry['log_tail'] = [x[:4000] for x in lines[-12:]]
                entry['firmware_stall'] = firmware_stall
            if (run_dir / 'dell-startup-script.sha256').exists():
                entry['dell_startup_script_sha256'] = (run_dir / 'dell-startup-script.sha256').read_text().strip()
            verdict['attempts'].append(entry)
            if passed or not firmware_stall:
                break
            print(f'attempt {attempt}: firmware stalled before boot selection; retrying', flush=True)
        verdict['passed'] = verdict['attempts'][-1]['passed']
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
        try:
            tmp_root.rmdir()
        except OSError:
            pass
        (out / 'verdict.json').write_text(json.dumps(verdict, indent=2) + '\n')

    print(('PASS ' if verdict.get('passed') else 'FAIL ') + str(out / 'verdict.json'))
    if not verdict.get('passed'):
        print('\n'.join(verdict.get('attempts', [{}])[-1].get('log_tail', [])), file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()

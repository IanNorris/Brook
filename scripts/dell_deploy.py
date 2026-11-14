#!/usr/bin/env python3
"""Deploy a pair built by build_dell_image.py to the Dell, only if it is fit to deploy.

Refuses unless <pair>/qemu-check/verdict.json says passed=true for this exact kernel/initrd, and the
build manifest is from a clean tree (override with --allow-dirty). Then runs
hardware_update_device.py --activate --restart with the payload ID taken from the manifest.

Usage (repo root, BROOK_ROOT set to the checkout holding the bridge session):
  python3 scripts/dell_deploy.py --session S --pair build/dell/pair-X --output build/dell/deploy-X
"""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KEY = Path('/data/Programming/Brook/shared/dell/hardware-update-key')

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--session', required=True); p.add_argument('--pair', type=Path, required=True)
p.add_argument('--output', type=Path, required=True); p.add_argument('--allow-dirty', action='store_true')
p.add_argument('--boot-timeout-s', type=int, default=240)
a = p.parse_args()

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
manifest = json.loads((a.pair / 'build-manifest.json').read_text())
verdict_path = a.pair / 'qemu-check' / 'verdict.json'
if not verdict_path.exists(): sys.exit(f'refusing: no QEMU verdict at {verdict_path}')
verdict = json.loads(verdict_path.read_text())
kernel, initrd = a.pair / 'kernel.elf', a.pair / 'INITRD.IMG'
if verdict.get('passed') is not True: sys.exit('refusing: QEMU check did not pass (rerun scripts/check_dell_image_qemu.py --force)')
for name, path in (('kernel_sha256', kernel), ('initrd_sha256', initrd)):
    if verdict.get(name) not in (None, sha(path)) or manifest.get(name) != sha(path):
        sys.exit(f'refusing: {name} does not match the checked/manifest pair')
if manifest.get('dirty') and not a.allow_dirty: sys.exit('refusing: built from a dirty tree')
cmd = [sys.executable, str(ROOT / 'scripts/hardware_update_device.py'), '--session', a.session, '--key-file', str(KEY),
       '--kernel', str(kernel), '--initrd', str(initrd), '--output', str(a.output), '--activate', '--restart',
       '--expected-payload-id', manifest['payload_id'], '--helper', 'system', '--boot-timeout-s', str(a.boot_timeout_s)]
sys.exit(subprocess.call(cmd))

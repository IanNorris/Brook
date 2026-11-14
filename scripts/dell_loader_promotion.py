#!/usr/bin/env python3
"""Replace the Dell's default guarded EFI loader, one checked step at a time (USB maintenance boot only).

See docs/dell-loader-promotion.md for the whole procedure. Steps (each is a separate invocation):

  inventory  read-only: upload the inventory tool, run it, compare the ESP with the expected table.
             Safe to repeat; it is also the dry run.
  stage      create-only: write the new loader as EFI/BOOT/BROOKT6.EFI (never touches existing files).
  promote    rename BOOTX64.EFI -> BROOKP5.EFI and BROOKT6.EFI -> BOOTX64.EFI, after re-running the
             inventory in the same invocation and passing every live hash to the transaction tool.

`stage` and `promote` need --execute; without it they print the plan and stop after the inventory.
Every step refuses unless the attached boot reports a USB maintenance payload (HWSTATUS.ELF) and its
inventory reports GUARD_INVENTORY_CLEAN. Results go to a new --output directory (receipt.json).
After an uncertain mutation, inspect the SAME job with hw_job; never rerun a mutating step blindly.

Usage (repo root, BROOK_ROOT set to the checkout holding the bridge session):
  python3 scripts/dell_loader_promotion.py inventory --session S --output DIR
  python3 scripts/dell_loader_promotion.py stage --session S --loader build/dell/loader-X --output DIR [--execute]
  python3 scripts/dell_loader_promotion.py promote --session S --loader build/dell/loader-X --output DIR [--execute]
"""
import argparse, hashlib, json, re, subprocess, sys, time, uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
SHARED = Path('/data/Programming/Brook/shared/dell')
KEY = SHARED / 'hardware-update-key'
MUSL = '/nix/store/h07xxi23n3vxrwxhz0v1v4m9a2cn12lc-x86_64-unknown-linux-musl-gcc-wrapper-15.2.0/bin/x86_64-unknown-linux-musl-gcc'
BRKMODE = '/media/system/EFI/BOOT/BRKMODE.ELF'
MAINTENANCE_PAYLOAD = re.compile(r'dell-usb-ssd-maintenance-[0-9]+')

# Last recorded state (USB maintenance boot 16dff15e..., 7 Nov 2026, inventory e25b310b... followed by the
# v5 repromotion fc2294ba...). Anything listed must match the live inventory exactly; guards are only
# required to be identical to each other and clean (they can legitimately change).
EXPECTED = {
    'EFI/BOOT/BOOTX64.EFI': 'c717c04856cacc2c4660a05fbc98076cad9eb8b7f839c25d0a733864820a0c00',  # v5 guarded default
    'EFI/BOOT/BROOKV5.EFI': 'c717c04856cacc2c4660a05fbc98076cad9eb8b7f839c25d0a733864820a0c00',  # v5 staged copy
    'EFI/BOOT/BROOKV4.EFI': '2576762f05a3f7401468659dd8e4d2dc7a279263cbaed4ce73089c089e941997',  # v4 guarded (old default)
    'EFI/BOOT/BROOKG2.EFI': '2576762f05a3f7401468659dd8e4d2dc7a279263cbaed4ce73089c089e941997',  # v4 staged copy
    'EFI/BOOT/BROOKOLD.EFI': 'cc1989c81db48d5ffc40d0f72e63faf55048090dbd297575a314a5f81d3fba09',  # original unguarded
    'EFI/BOOT/BROOKF5.EFI': '8f29b69f638c144fd29578c08f9062095c02457bf3d8b2578f6b8258887161b1',  # firmware F5 entry
    'EFI/BOOT/BRKMODE.ELF': 'ba2c6ca74e21e1ba2c74b5f42679689df091adc22c5e601b4edde8b260fa1351',  # RAM exec helper
    'RECOV2/KERNEL.ELF': 'b7e2684e12096accff3380c8efdf0aa5c3531408712533f3fba3e6a4065b2c37',
    'RECOV2/INITRD.IMG': '068876ff817d7c9adfb87f937d523dd726d600492130c66000e8bb7d4f979653',
    'RECOV2/SELECT.BIN': 'fc624637b5c434ae78fb247812c5698f92317f9a85c66bc593335537dc5a311a',
}
ABSENT_BEFORE = ('EFI/BOOT/BROOKP5.EFI', 'EFI/BOOT/BROOKNEW.EFI')
TOOLS = {  # name -> (source, extra flags, C++?)
    'inventory': ('tools/hardware_guard_inventory.cpp', [], True),
    'stage': ('tools/hardware_efi_inventory_stage.c', ['-DBROOK_EFI_GUARD_T6_STAGE'], False),
    'promote': ('tools/hardware_guard_default_stage.c', ['-DBROOK_GUARD_REPROMOTE_NEXT'], False),
}


def sha(data): return hashlib.sha256(data).hexdigest()


class Step:
    def __init__(self, a):
        from hardware import Hardware
        self.a, self.h = a, Hardware()
        a.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.receipt = {'step': a.step, 'session': a.session, 'execute': a.execute}
        self.save()

    def save(self, **kw):
        self.receipt.update(kw)
        (self.a.output / 'receipt.json').write_text(json.dumps(self.receipt, indent=2) + '\n')

    def run(self, argv, label, timeout=120):
        tx = uuid.uuid4().hex
        self.save(**{f'{label}_job': tx})
        job = self.h.guest_run(self.a.session, tx, argv, cwd='/', uid=0, wait_s=2)
        end = time.monotonic() + timeout
        while job.get('state') not in ('completed', 'failed') and time.monotonic() < end:
            job = self.h.job(self.a.session, tx, wait_s=2)
        out = {'state': job.get('state'), 'exit': job.get('exit'),
               'stdout': job.get('stdout', {}).get('text', ''), 'stderr': job.get('stderr', {}).get('text', '')}
        self.save(**{f'{label}_result': out})
        if out['state'] != 'completed':
            sys.exit(f'{label} job {tx} did not complete; inspect it with hw_job before anything else')
        return out

    def check_boot(self):
        session = self.h.load_session(self.a.session)
        doctor = self.h.doctor(self.a.session)
        if doctor.get('probe_stage') != 'verified' or not doctor.get('identity_matches'):
            sys.exit('boot probe not verified/matching; re-attach first')
        status = self.run(['/boot/HWSTATUS.ELF'], 'status', 30)
        m = re.search(r'HARDWARE_PAYLOAD_ID (\S+)', status['stdout'])
        self.save(guest_boot_id=session['guest_boot_id'], payload_id=m and m.group(1))
        if not m or not MAINTENANCE_PAYLOAD.fullmatch(m.group(1)):
            sys.exit(f'not a USB maintenance boot (payload {m and m.group(1)}); the guard files are held by a desktop kernel')

    def upload_executable(self, label, path):
        """Put a file in current-boot RAM; for executables, mark it executable with BRKMODE."""
        tx = uuid.uuid4().hex
        result = self.h.transfer(self.a.session, tx, 'put', str(KEY), str(self.a.output / f'{label}-upload'), source=str(path))
        self.save(**{f'{label}_upload': {'transaction_id': tx, 'state': result['state'], 'sha256': sha(path.read_bytes())}})
        if result['state'] != 'completed':
            sys.exit(f'upload {tx} state {result["state"]}; inspect its receipt, do not retry into the same directory')
        return tx, f'/data/.brook-transfer/{tx}.bin'

    def tool(self, name):
        source, flags, cxx = TOOLS[name]
        out = self.a.output / f'{name}.elf'
        cc = MUSL[:-3] + 'g++' if cxx else MUSL
        subprocess.run([cc, *(['-x', 'c++'] if cxx else []), '-static', '-O2', '-Wall', '-Wextra', '-Werror', *flags,
                        str(ROOT / source), '-o', str(out)], check=True)
        tx, guest = self.upload_executable(name, out)
        mode = self.run([BRKMODE, tx, sha(out.read_bytes())], f'{name}_brkmode', 30)
        if (mode['exit'] or {}).get('code') != 0 or 'RAM_EXEC_STAGE_OK' not in mode['stdout']:
            sys.exit(f'BRKMODE failed for {name}: {mode["stderr"]}')
        return guest

    def inventory(self):
        identity = None
        for line in (SHARED / 'guard-loader-options.txt').read_text().splitlines():
            if line.startswith('BROOK_GUARD_IDENTITY:STRING='):
                identity = line.split('=', 1)[1].strip()
        guest = self.tool('inventory')
        result = self.run([guest, identity], 'inventory', 60)
        seen = {}
        for line in result['stdout'].splitlines():
            parts = line.split()
            if len(parts) == 3 and re.fullmatch(r'[0-9a-f]{64}', parts[1]):
                seen[parts[0]] = parts[1]
            elif len(parts) == 2 and parts[1] in ('absent', 'unreadable'):
                seen[parts[0]] = parts[1]
        clean = 'GUARD_INVENTORY_CLEAN' in result['stdout'] and (result['exit'] or {}).get('code') == 0
        expected, state = dict(EXPECTED), 'v5-default'
        if seen.get('EFI/BOOT/BROOKP5.EFI') == EXPECTED['EFI/BOOT/BOOTX64.EFI']:
            # Promoted: the v5 default now lives in BROOKP5 and BOOTX64 must equal the staged BROOKT6.
            expected['EFI/BOOT/BOOTX64.EFI'], state = seen.get('EFI/BOOT/BROOKT6.EFI'), 'promoted'
        diffs = {k: {'expected': v, 'live': seen.get(k)} for k, v in expected.items() if seen.get(k) != v}
        self.save(inventory=seen, guard_clean=clean, differences=diffs, esp_state=state)
        print(json.dumps({'esp_state': state, 'guard_clean': clean, 'differences': diffs,
                          'other': {k: v for k, v in seen.items() if k not in EXPECTED}}, indent=2))
        if not clean:
            sys.exit('inventory not clean (guards unreadable, differing or not admitting normal boot)')
        if diffs:
            sys.exit('live ESP differs from the expected table; stop and review before any mutation')
        return seen


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('step', choices=('inventory', 'stage', 'promote'))
    p.add_argument('--session', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--loader', type=Path, help='build_dell_loader.py output directory (stage/promote)')
    p.add_argument('--execute', action='store_true', help='perform the mutation (stage/promote)')
    a = p.parse_args()
    loader = None
    if a.step != 'inventory':
        if not a.loader: p.error('--loader is required')
        manifest = json.loads((a.loader / 'manifest.json').read_text())
        loader = (a.loader / 'BROOKGUARD.efi').read_bytes()
        if sha(loader) != manifest['sha256'] or manifest.get('dirty'):
            sys.exit('loader does not match its manifest or was built from a dirty tree')
    s = Step(a)
    s.check_boot()
    seen = s.inventory()
    if s.receipt['esp_state'] == 'promoted' and a.step != 'inventory':
        sys.exit('ESP is already promoted (BROOKP5.EFI holds the v5 default); nothing to do')
    if a.step == 'inventory':
        return
    new = sha(loader)
    if a.step == 'stage':
        if seen.get('EFI/BOOT/BROOKT6.EFI') == new:
            print('BROOKT6.EFI already holds this loader; nothing to stage'); return
        if seen.get('EFI/BOOT/BROOKT6.EFI') != 'absent':
            sys.exit('BROOKT6.EFI exists with different content; review before continuing')
        if not a.execute:
            print(f'plan: create EFI/BOOT/BROOKT6.EFI sha256 {new} (rerun with --execute)'); return
        stage = s.tool('stage')
        tx, guest_loader = s.upload_executable('loader', a.loader / 'BROOKGUARD.efi')
        result = s.run([stage, guest_loader, new], 'stage_run', 60)
        if 'EFI_INVENTORY_STAGE_OK' not in result['stdout']:
            sys.exit('stage failed: ' + result['stderr'])
        print('staged BROOKT6.EFI; run `inventory` again to confirm, then `promote`')
        return
    # promote
    if seen.get('EFI/BOOT/BROOKT6.EFI') != new:
        sys.exit('BROOKT6.EFI is not the expected loader (stage it first)')
    for name in ABSENT_BEFORE:
        if seen.get(name) != 'absent':
            sys.exit(f'{name} already exists; inspect before promoting')
    order = ['EFI/BOOT/BOOTX64.EFI', 'EFI/BOOT/BROOKT6.EFI', 'EFI/BOOT/BROOKF5.EFI', 'RECOV2/KERNEL.ELF',
             'RECOV2/INITRD.IMG', 'RECOV2/SELECT.BIN', 'RECOVERY/GUARD0.BIN', 'RECOVERY/GUARD1.BIN',
             'EFI/BOOT/BROOKOLD.EFI', 'EFI/BOOT/BROOKG2.EFI', 'EFI/BOOT/BROOKV4.EFI', 'EFI/BOOT/BROOKV5.EFI']
    hashes = [seen[k] for k in order]
    s.save(promote_hashes=dict(zip(order, hashes)))
    if not a.execute:
        print('plan: BOOTX64.EFI -> BROOKP5.EFI, BROOKT6.EFI -> BOOTX64.EFI with the hashes in receipt.json (rerun with --execute)')
        return
    promote = s.tool('promote')
    result = s.run([promote, *hashes], 'promote_run', 60)
    print(result['stdout'] + result['stderr'])
    if 'GUARD_DEFAULT_OK' not in result['stdout']:
        sys.exit('promotion did not report GUARD_DEFAULT_OK; DO NOT restart or retry. Inspect the job and the ESP '
                 '(inventory) first; see docs/dell-loader-promotion.md "Interrupted promotion"')


if __name__ == '__main__':
    main()

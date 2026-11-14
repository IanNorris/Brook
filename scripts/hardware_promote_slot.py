#!/usr/bin/env python3
"""Re-select an older, still-present SSD boot slot (rollback) from a maintenance boot.

Launches /boot/HWUPSYS.ELF on the attached boot, then uses the BRPROMOT protocol to make the
verified older pair (exact kernel/initrd identity required) the newest selection, without rewriting
any payload. One attempt only: the output directory is created exclusively and holds receipt.json
with the transaction ID. On any uncertain outcome inspect that SAME job with hw_job; never rerun.

Usage (repo root): python3 scripts/hardware_promote_slot.py --session S --key-file K --slot 0|1
    --kernel PAIR/kernel.elf --initrd PAIR/INITRD.IMG --output DIR [--expected-generation N] [--activate]
"""
import argparse, hashlib, json, re, sys, time, uuid
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
from hardware import Hardware
from hardware_update_protocol import promote, read_payload, KERNEL_MAX, INITRD_MAX

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--session', required=True); p.add_argument('--key-file', type=Path, required=True)
p.add_argument('--slot', type=int, choices=(0, 1), required=True)
p.add_argument('--kernel', type=Path, required=True); p.add_argument('--initrd', type=Path, required=True)
p.add_argument('--output', type=Path, required=True); p.add_argument('--expected-generation', type=int)
p.add_argument('--port', type=int, default=1235); p.add_argument('--activate', action='store_true')
a = p.parse_args()

a.output.mkdir(parents=True, exist_ok=False, mode=0o700)
receipt = {}
def save(**kw):
    receipt.update(kw)
    (a.output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

h = Hardware()
session = h.load_session(a.session); boot = session['guest_boot_id']
doctor = h.doctor(a.session)
if doctor.get('probe_stage') != 'verified' or not doctor.get('identity_matches'):
    sys.exit('boot probe not verified/matching')
key = a.key_file.read_text()[:33]
if not re.fullmatch(r'[0-9a-f]{32}', key): sys.exit('bad key file')
kernel = read_payload(a.kernel, KERNEL_MAX); initrd = read_payload(a.initrd, INITRD_MAX)
tx = uuid.uuid4().hex
save(state='prepared', session=a.session, boot_id=boot, transaction_id=tx, slot=a.slot,
     expected_generation=a.expected_generation, activate=a.activate,
     kernel_sha256=hashlib.sha256(kernel).hexdigest(), initrd_sha256=hashlib.sha256(initrd).hexdigest())
job = h.guest_run(a.session, tx, ['/boot/HWUPSYS.ELF', boot, tx], cwd='/tmp', uid=0, wait_s=2)
save(state='helper_dispatched', job_initial=job)
end = time.monotonic() + 60
while 'HWUPDATE_LISTENING' not in job.get('stdout', {}).get('text', ''):
    if job.get('state') in ('completed', 'failed') or time.monotonic() > end:
        save(state='helper_not_listening', job=job); sys.exit('helper did not listen; inspect job ' + tx)
    job = h.job(a.session, tx, wait_s=2)
save(state='helper_listening')
try:
    result = promote(session['host'], a.port, key, boot, tx, a.slot, kernel, initrd, a.activate,
                     expected_generation=a.expected_generation)
except Exception as e:
    save(state='needs_inspection', error=repr(e)); raise
save(state='promote_reply', result=result)
end = time.monotonic() + 60
while job.get('state') not in ('completed', 'failed') and time.monotonic() < end:
    job = h.job(a.session, tx, wait_s=2)
save(state=result.get('state'), job_final=job)
print(json.dumps({k: receipt[k] for k in ('state', 'transaction_id')} | {'result': result}, indent=2))

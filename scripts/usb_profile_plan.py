"""Host-side admission for the desktop's signed selected profile plans."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/nix-install'))
from stage_profile import MAX_PLAN, validate_plan
from stage_selected_space import validate_usage
from stage_selected_launchers import validate_launcher_ids


def checked_plan(directory, public_key):
    key_bytes = public_key.read_bytes()
    if len(key_bytes) > 4096:
        raise ValueError("Profile public key exceeds its size limit")
    with (directory / 'stage-plan.json').open('rb') as stream:
        document = stream.read(MAX_PLAN + 1)
    with (directory / 'stage-plan.sig').open('rb') as stream:
        signature = stream.read(65)
    if len(document) > MAX_PLAN or len(signature) != 64:
        raise ValueError('Invalid profile plan or Ed25519 signature length')
    with tempfile.TemporaryDirectory() as scratch:
        data, sig, pub = (Path(scratch) / name for name in ('document', 'signature', 'key'))
        data.write_bytes(document)
        sig.write_bytes(signature)
        pub.write_bytes(key_bytes)
        key = subprocess.check_output(['openssl', 'pkey', '-pubin', '-in', pub, '-outform', 'DER'])
        if len(key) != 44 or not key.startswith(bytes.fromhex('302a300506032b6570032100')):
            raise ValueError('Expected an Ed25519 public plan key')
        subprocess.run(['openssl', 'pkeyutl', '-verify', '-pubin', '-rawin', '-inkey',
                        pub, '-in', data, '-sigfile', sig],
                       check=True, stdout=subprocess.DEVNULL)
    plan = json.loads(document)
    if plan['format'] != 3 or plan['cache']['brook'] is not None:
        raise ValueError('USB system requires a signed selected plan with bridge-cache delivery')
    validate_plan({**plan, 'format': 2})
    validate_usage(plan)
    validate_launcher_ids(plan['profile']['launcherIds'])
    profile = Path('/nix/store') / plan['profile']['root']
    for name in ('bash', 'brook-chromium', 'brook-gallery', 'hardware-dbus',
                 'hardware-message', 'waylandd', 'brook-stage-profile'):
        binary = profile / 'bin' / name
        if not binary.is_file() or not binary.stat().st_mode & 0o111:
            raise ValueError(f'Selected profile must provide {name} for USB desktop boot')
    return plan, hashlib.sha256(document).hexdigest(), key_bytes

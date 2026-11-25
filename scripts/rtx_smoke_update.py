#!/usr/bin/env python3
"""Remote RTX boot-set and signed desktop profile updates over the v3 bridge.

  signer-public --key KEY                 print the Ed25519 public key (hex) for
                                          build_rtx_smoke_files.py --update-signer
  status  --session S                     loader set/trial, gate state, PREV payload
  push    --session S --files DIR --key KEY --output NEW_DIR [--restart [--confirm]]
  confirm --session S                     mark the running root set good (drop TRIAL.RUN)
  profile-bootstrap --session S --output DIR
  profile --session S --profile-plan DIR --public-key FILE --output DIR
  profile --session S --rollback-profile RECEIPT --output DIR

KEY is the host-only hardware update key (shared/dell/hardware-update-key). The
signing seed is SHA-256("brook-rtx-stick-update-signing-v1\\0" || KEY); only the
public half is in the stick's initrd. The key never leaves this process.

push signs a binary manifest of the file set (manifest.json sha256 per file),
launches /boot/HWRTXUP.ELF as a boot-pinned guest job, streams the set over a v3
operator-authenticated connection (port 1235), and records receipt.json in the
new --output directory after every step. The guest verifies the signature and
every digest in RAM, then its kernel gate allows writes to the boot stick's
own partition only. --restart sends hw_power restart (ACPI reset), waits for a
new boot id on the same endpoint, attaches a new session and checks the new
payload ID and the loader's trial state. --confirm then removes TRIAL.RUN so
the new set stays selected; without it, the next reset falls back to PREV/.
Mutations are never retried: inspect receipt.json and the guest job IDs first.
"""
import argparse, hashlib, json, os, re, socket, struct, sys, time, uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]

DOMAIN = b'BROOK-RTX-STICK-MANIFEST-V1\0'
SEED_LABEL = b'brook-rtx-stick-update-signing-v1\0'
NAMES = ('EFI/BOOT/BOOTX64.EFI', 'KERNEL/BROOK.ELF', 'INITRD.IMG', 'BROOK.CFG', 'README.TXT', 'manifest.json', 'SET.BIN')
UPDATE, STICK = 2, 3


def signing_key(key_file):
    from nacl.signing import SigningKey
    data = Path(key_file).read_bytes()
    if len(data) < 16:
        raise ValueError('update key file is too short')
    return SigningKey(hashlib.sha256(SEED_LABEL + data).digest())


def load_set(files):
    """File bytes in NAMES order, checked against manifest.json; payload ID."""
    files = Path(files)
    manifest_raw = (files / 'manifest.json').read_bytes()
    manifest = json.loads(manifest_raw)
    if manifest.get('format') != 'brook-rtx-smoke-v3' or manifest.get('fat_label') != 'BROOK_RTX':
        raise ValueError('not a brook-rtx-smoke-v3 BROOK_RTX file set')
    blobs = []
    for name in NAMES:
        data = (files / name).read_bytes()
        if name != 'manifest.json':
            row = manifest['files'][name]
            if len(data) != row['bytes'] or hashlib.sha256(data).hexdigest() != row['sha256']:
                raise ValueError(f'{name} differs from manifest.json')
        blobs.append(data)
    return blobs, manifest


def binary_manifest(blobs, payload_id, commit):
    def text(value, size):
        raw = value.encode()
        if not raw or len(raw) >= size or any(c < 0x21 or c > 0x7e for c in raw):
            raise ValueError(f'bad manifest text {value!r}')
        return raw.ljust(size, b'\0')
    out = b'BRKRTXM1' + struct.pack('<II', 1, len(NAMES)) + text(payload_id, 48) + text('BROOK_RTX', 16)
    out += text(commit or '-', 48)
    for name, data in zip(NAMES, blobs):
        out += text(name, 40) + struct.pack('<Q', len(data)) + hashlib.sha256(data).digest()
    assert len(out) == 128 + 80 * len(NAMES)
    return out


def sign(key, manifest):
    return key.sign(DOMAIN + manifest).signature


STORE_DOMAIN = b'BROOK-RTX-STORE-MANIFEST-V1\0'


def store_manifest(first_lba, sectors, size, digest_hex, store_id):
    """BRKSTOR1 store-slot manifest (tools/hardware_rtx_update.c receive_store)."""
    ident = store_id.encode()
    if not 0 < len(ident) <= 48 or size % 4096 or size > sectors * 512 or first_lba < 2048:
        raise ValueError('bad store manifest fields')
    return b'BRKSTOR1' + struct.pack('<QQQ', first_lba, sectors, size) + bytes.fromhex(digest_hex) + ident.ljust(48, b'\0')


def line(stream):
    raw = bytearray()
    while len(raw) < 1024:
        value = stream.recv(1)
        if not value:
            raise ConnectionError('receiver disconnected; inspect the guest job before anything else')
        if value == b'\n':
            return json.loads(raw)
        raw += value
    raise ValueError('oversize receiver line')


def install_timeout(blobs):
    """Seconds to wait for the final 'updated' line after ACTIVATE.

    On the real stick (xHCI bulk writes, PREV/ rebuild, read-back and a fresh
    read-only verify) a ~10 MB set took longer than the old fixed 180 s. Bound
    it by size at a pessimistic 50 KB/s for the ~3x bytes the guest writes and
    reads back, never below 300 s; the guest's own deadline is the upper limit.
    """
    total = sum(len(b) for b in blobs)
    return max(300, min(1800, 60 + (3 * total) // 50_000))


def stream_set(host, port, operator, boot, tx, manifest, signature, blobs, *, tamper=None, replies=None,
               final_timeout=300):
    """One upload; tamper is for negative tests only ('signature' or 'file').

    replies is filled in place so a caller keeps the lines received before a
    timeout or disconnect."""
    from binary_bridge import binary_stream, finish_request, require_response_end
    replies = [] if replies is None else replies
    with socket.create_connection((host, port), timeout=30) as connection:
        connection.settimeout(180)
        stream, _ = binary_stream(connection, operator, UPDATE, STICK, boot, tx)
        stream.sendall(b'BRRTXUP1' + boot.encode() + tx.encode())
        replies.append(line(stream))
        if replies[-1].get('state') != 'ready':
            return replies
        if tamper == 'signature':
            signature = bytes([signature[0] ^ 1]) + signature[1:]
        stream.sendall(struct.pack('<I', len(manifest)) + manifest + signature)
        replies.append(line(stream))
        if replies[-1].get('state') != 'manifest_ok':
            return replies
        for index, data in enumerate(blobs):
            if tamper == 'file' and index == 1:
                data = data[:-1] + bytes([data[-1] ^ 0xff])
            stream.sendall(data)
        replies.append(line(stream))
        if replies[-1].get('state') != 'staged':
            return replies
        stream.sendall(b'ACTIVATE')
        finish_request(stream)
        stream.settimeout(final_timeout)
        replies.append(line(stream))
        require_response_end(stream)
    return replies


class Run:
    def __init__(self, bridge, output):
        self.bridge, self.output, self.receipt = bridge, Path(output) if output else None, {}

    def save(self, **changes):
        self.receipt.update(changes)
        if not self.output:
            return
        temporary = self.output / 'receipt.new'
        with temporary.open('w') as stream:
            os.chmod(temporary, 0o600)
            json.dump(self.receipt, stream, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        temporary.replace(self.output / 'receipt.json')

    def job(self, session, argv, predicate=lambda r: r.get('state') == 'completed', timeout=120, name='job', uid=0):
        ident = uuid.uuid4().hex
        self.save(**{name + '_id': ident})
        self.bridge.guest_run(session, ident, argv, cwd='/tmp', uid=uid, wait_s=2)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            result = self.bridge.job(session, ident, wait_s=2)
            if predicate(result):
                return ident, result
            if result.get('state') in ('completed', 'failed', 'rejected', 'lost'):
                return ident, result
            time.sleep(0.1)
        raise TimeoutError(f'inspect guest job {ident}')

    def status(self, session):
        _, result = self.job(session, ['/boot/HWRTXUP.ELF', '--status'], name='status_job')
        text = result.get('stdout', {}).get('text', '')
        fields = dict(re.findall(r'(\w+)=(\S+)', text.split('HWRTXUP_STATUS', 1)[-1]))
        root = re.search(r'HWRTXUP_ROOT_PAYLOAD (\S+)', text)
        fields['root_payload'] = root[1] if root else None
        fields['exit'] = result.get('exit', {}).get('code')
        return fields

    def payload_id(self, session):
        _, result = self.job(session, ['/boot/HWSTATUS.ELF'], name='hwstatus_job')
        match = re.search(r'HARDWARE_PAYLOAD_ID (\S+)', result.get('stdout', {}).get('text', ''))
        return match[1] if match else None

    def confirm(self, session):
        _, result = self.job(session, ['/boot/HWRTXUP.ELF', '--confirm'], name='confirm_job')
        return result.get('exit', {}).get('code'), result.get('stdout', {}).get('text', '').strip()

    def restart(self, session, data, operator_file, boot, timeout):
        state = self.bridge.command(session, 'bridge hardware caps').get('usb_system', {})
        if state.get('profile_transfer_ready'):
            _, job = self.job(session, ['/boot/bin/bash', '-c', 'test ! -e /nix/var/.brook-profile-lock'],
                              name='profile_status', uid=1000)
            if job.get('exit', {}).get('code') != 0:
                raise RuntimeError('Profile job lock is active or unavailable; restart refused')
        power_id = uuid.uuid4().hex
        self.save(state='restart_dispatched', power_request_id=power_id)
        status = self.bridge.power(session, power_id, 'restart')
        end = time.monotonic() + 90
        while status.get('state') != 'ready':
            if status.get('state') in ('failed', 'unsupported', 'cancelled') or time.monotonic() > end:
                raise RuntimeError(f'restart not ready: {status}')
            time.sleep(0.5)
            status = self.bridge.power(session, power_id, 'status')
        try:
            self.save(power_commit=self.bridge.power(session, power_id, 'commit'))
        except (OSError, TimeoutError, ConnectionError, RuntimeError):
            self.save(commit_reply_missing=True)  # the reset may drop its own reply
        self.save(state='awaiting_new_boot')
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                _, caps = self.bridge.probe_v3(data['host'], data['port'], operator_file, timeout=4)
            except (OSError, ValueError, RuntimeError):
                time.sleep(1)
                continue
            if caps.get('boot_id') and caps['boot_id'] != boot:
                fresh = self.bridge.attach(data['host'], data['port'], auth_mode='v3', operator_key_file=operator_file)
                self.save(state='new_boot_attached', new_session=fresh['session'], new_guest_boot_id=fresh['guest_boot_id'],
                          new_build_id=fresh['guest'].get('build_id'), new_initrd_sha256=fresh['guest'].get('initrd_sha256'))
                return fresh
            time.sleep(1)
        raise TimeoutError('no new boot observed; inspect power request ' + power_id + ' (Ian: press reset; PREV/ boots)')

    def profile_target(self, session):
        guest = self.bridge.load_session(session)
        if guest.get('auth_mode') != 'v3':
            raise RuntimeError('Profile updates require a v3 session')
        state = self.bridge.command(session, 'bridge hardware caps').get('usb_system', {})
        if (not state.get('mounted') or not state.get('window_open') or state.get('uncertain')
                or not state.get('profile_transfer_ready')):
            raise RuntimeError('Profile updates require a writable BROOK_SYSTEM with its transfer bind active')
        return state

    def profile_bootstrap(self, session):
        self.profile_target(session)
        self.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        _, job = self.job(session, ['/boot/bin/bash', '/boot/USBSTAGE.RC', 'initialize'],
                          timeout=180, name='profile_bootstrap')
        self.save(profile_job=job)
        if job.get('exit', {}).get('code') != 0:
            raise RuntimeError(f'Profile bootstrap failed: {job}')
        self.save(state='profile_initialized')
        return self.receipt

    def profile(self, args):
        from dell_deploy import stage_profile
        self.profile_target(args.session)
        args.profile_plan_public_key = args.public_key
        args.push_profile_cache = bool(args.profile_plan)
        if args.rollback_profile:
            previous = json.loads(args.rollback_profile.read_text())
            if previous.get('destination') != 'usb-system':
                raise ValueError('Expected a USB system profile receipt')
        journal = stage_profile(args, bridge=self.bridge, bulk_port=args.bulk_port, usb_system=True)
        return journal.receipt

    def store(self, session, store_dir, key_file, first_lba, sectors, upload_port):
        """Write a store image into a slot over the network; the running store
        and record stay untouched (a later signed set switches the record)."""
        if self.bridge.command(session, 'bridge hardware caps').get('usb_system', {}).get('configured'):
            raise RuntimeError('Store slots are only available to the fallback stick')
        meta = json.loads((Path(store_dir) / 'store-manifest.json').read_text())
        image = Path(store_dir) / 'profile/STORE.IMG'
        size = image.stat().st_size
        if meta.get('format') != 'brook-rtx-live-store/1' or size != meta['bytes']:
            raise RuntimeError('store manifest does not match STORE.IMG')
        store_id = 'store-' + meta['sha256'][:16]
        binary = store_manifest(first_lba, sectors, size, meta['sha256'], store_id)
        signature = signing_key(key_file).sign(STORE_DOMAIN + binary).signature
        data = self.bridge.load_session(session)
        if data.get('auth_mode') != 'v3':
            raise RuntimeError('store updates require a v3 session')
        from binary_bridge import receiver_key, binary_stream, finish_request, require_response_end
        operator = receiver_key(data, None)
        boot = data['guest_boot_id']
        if self.output:
            self.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        tx = uuid.uuid4().hex
        record = f"lba={first_lba} sectors={sectors} bytes={size} sha256={meta['sha256']}\n"
        self.save(state='prepared', mode='store', session=session, guest_boot_id=boot, transaction_id=tx,
                  store=str(Path(store_dir).resolve()), bytes=size, sha256=meta['sha256'], first_lba=first_lba,
                  sectors=sectors, record=record)
        self.bridge.guest_run(session, tx, ['/boot/HWRTXUP.ELF', boot, tx, '--store'], cwd='/tmp', uid=0, wait_s=2)
        end = time.monotonic() + 60
        while 'HWUPDATE_LISTENING' not in self.bridge.job(session, tx, wait_s=2).get('stdout', {}).get('text', ''):
            if time.monotonic() > end:
                raise RuntimeError('receiver did not listen')
            time.sleep(0.1)
        replies = []
        started = time.monotonic()
        try:
            with socket.create_connection((data['host'], upload_port), timeout=30) as connection:
                connection.settimeout(300)
                stream, _ = binary_stream(connection, operator, UPDATE, STICK, boot, tx)
                stream.sendall(b'BRRTXST1' + boot.encode() + tx.encode())
                replies.append(line(stream))
                stream.sendall(binary + signature)
                replies.append(line(stream))
                self.save(replies=replies)
                if replies[-1].get('state') != 'open':
                    self.save(state='refused'); return self.receipt
                sent = 0
                with image.open('rb') as f:
                    for chunk in iter(lambda: f.read(4 << 20), b''):
                        stream.sendall(chunk); sent += len(chunk)
                        if sent % (512 << 20) < len(chunk):
                            self.save(sent=sent, send_seconds=round(time.monotonic() - started, 1))
                finish_request(stream)
                stream.settimeout(max(600, size // (5 << 20)))
                replies.append(line(stream))
                require_response_end(stream)
        except (OSError, ConnectionError, ValueError, RuntimeError) as error:
            self.save(upload_error=str(error))
        self.save(replies=replies, seconds=round(time.monotonic() - started, 1))
        job = self.bridge.job(session, tx, wait_s=5)
        final = replies[-1] if replies else {}
        ok = final.get('state') == 'updated' and final.get('verified') is True
        out = None
        if ok:
            meta2 = dict(meta, record=record, slot={'first_lba': first_lba, 'sectors': sectors})
            out = Path(self.output or store_dir) / 'store-manifest-slot.json'
            out.write_text(json.dumps(meta2, indent=2) + '\n')
        self.save(state='stored' if ok else 'needs_inspection', job_stdout=(job.get('stdout') or {}).get('text'),
                  slot_manifest=str(out) if out else None)
        return self.receipt

    def push(self, session, files, key_file, upload_port, restart, confirm, boot_timeout, tamper=None):
        blobs, manifest = load_set(files)
        payload_id = manifest['payload_id']
        key = signing_key(key_file)
        binary = binary_manifest(blobs, payload_id, manifest.get('commit', ''))
        signature = sign(key, binary)
        data = self.bridge.load_session(session)
        if data.get('auth_mode') != 'v3':
            raise RuntimeError('stick updates require a v3 session')
        from binary_bridge import receiver_key
        operator = receiver_key(data, None)
        boot = data['guest_boot_id']
        doctor = self.bridge.doctor(session)
        if doctor.get('probe_stage') != 'verified' or not doctor.get('reachable') or not doctor.get('identity_matches'):
            raise RuntimeError('session is not reachable on its pinned boot')
        if self.output:
            self.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        tx = uuid.uuid4().hex
        self.save(state='prepared', session=session, guest_boot_id=boot, transaction_id=tx, files=str(Path(files).resolve()),
                  payload_id=payload_id, manifest_sha256=hashlib.sha256(binary).hexdigest(),
                  file_sha256={n: hashlib.sha256(b).hexdigest() for n, b in zip(NAMES, blobs)},
                  signer_public=key.verify_key.encode().hex(), tamper=tamper)
        self.save(before=self.status(session))
        ident = tx
        self.bridge.guest_run(session, ident, ['/boot/HWRTXUP.ELF', boot, tx], cwd='/tmp', uid=0, wait_s=2)
        end = time.monotonic() + 60
        while True:
            job = self.bridge.job(session, ident, wait_s=2)
            if 'HWUPDATE_LISTENING' in job.get('stdout', {}).get('text', ''):
                break
            if job.get('state') in ('completed', 'failed', 'rejected', 'lost') or time.monotonic() > end:
                raise RuntimeError(f'receiver did not listen: {job}')
            time.sleep(0.1)
        self.save(state='receiver_listening')
        replies = []
        final_timeout = install_timeout(blobs)
        started = time.monotonic()
        try:
            stream_set(data['host'], upload_port, operator, boot, tx, binary, signature, blobs, tamper=tamper,
                       replies=replies, final_timeout=final_timeout)
        except (OSError, ConnectionError, ValueError, RuntimeError) as error:
            self.save(upload_error=str(error))
        self.save(state='upload_finished', replies=replies, upload_seconds=round(time.monotonic() - started, 1),
                  final_timeout_s=final_timeout)
        # The guest job outlives a lost upload reply: it may still be writing.
        # Wait for it (its own deadline is 600 s from start) before judging.
        end = time.monotonic() + final_timeout + 120
        while True:
            job = self.bridge.job(session, ident, wait_s=2)
            if job.get('state') in ('completed', 'failed', 'rejected', 'lost') or time.monotonic() > end:
                break
        stdout = job.get('stdout', {}).get('text') or ''
        self.save(job={'state': job.get('state'), 'exit': job.get('exit'), 'stdout': stdout,
                       'stderr': job.get('stderr', {}).get('text')}, job_seconds=round(time.monotonic() - started, 1))
        final = replies[-1] if replies else {}
        after = self.status(session)
        self.save(after=after)
        job_updated = job.get('state') == 'completed' and job.get('exit', {}).get('code') == 0 and \
            f'HWRTXUP_UPDATED payload={payload_id} ' in stdout
        if job_updated and final.get('state') != 'updated':
            # The 'updated' line was lost (slow writes or a dropped stream), but
            # the guest job finished its verified install. Accept it only when
            # the stick now reports the new root payload with a pending trial.
            recovered = after.get('root_payload') == payload_id and 'B' in (after.get('markers') or '') and \
                after.get('exit') == 0
            self.save(receipt_recovered=recovered)
            if recovered:
                match = re.search(r'HWRTXUP_UPDATED (.*)', stdout)
                final = {'state': 'updated', 'recovered_from_job': True,
                         **dict(re.findall(r'(\w+)=(\S+)', match[1] if match else ''))}
        ok = final.get('state') == 'updated' and job_updated
        if not ok:
            # 'refused' promises an unmodified stick: never claim it when the
            # job reports an install or the stick's root payload changed.
            modified = final.get('storage_modified') or job_updated or \
                after.get('root_payload') != (self.receipt.get('before') or {}).get('root_payload') or \
                job.get('state') != 'completed'
            self.save(state='needs_inspection' if modified else 'refused')
            return self.receipt
        self.save(state='updated', update=final)
        if not restart:
            return self.receipt
        fresh = self.restart(session, data, data['operator_key_file'], boot, boot_timeout)
        new = fresh['session']
        observed = self.payload_id(new)
        stick = self.status(new)
        self.save(new_payload_id=observed, new_status=stick)
        if observed != payload_id or stick.get('set') != 'root' or stick.get('trial') != 'started':
            self.save(state='new_boot_unexpected')
            return self.receipt
        self.save(state='new_boot_verified')
        if confirm:
            code, text = self.confirm(new)
            self.save(confirm={'exit': code, 'stdout': text}, state='confirmed' if code == 0 else 'confirm_failed',
                      confirmed_status=self.status(new))
        return self.receipt


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    s = sub.add_parser('signer-public'); s.add_argument('--key', type=Path, required=True)
    for name in ('status', 'confirm'):
        s = sub.add_parser(name); s.add_argument('--session', required=True)
    s = sub.add_parser('store', help='write a live-store image into a store slot over the network')
    s.add_argument('--session', required=True); s.add_argument('--store', type=Path, required=True)
    s.add_argument('--key', type=Path, required=True); s.add_argument('--output', type=Path, required=True)
    s.add_argument('--first-lba', type=int, required=True); s.add_argument('--sectors', type=int, required=True)
    s.add_argument('--upload-port', type=int, default=1235)
    s = sub.add_parser('profile-bootstrap', help='initialize profile switching using the signed boot key')
    s.add_argument('--session', required=True)
    s.add_argument('--output', type=Path, required=True)
    s = sub.add_parser('profile-recover', help='clear interrupted staging from an earlier boot')
    s.add_argument('--session', required=True)
    s.add_argument('--output', type=Path, required=True)
    s = sub.add_parser('profile', help='apply or roll back a signed profile on BROOK_SYSTEM')
    s.add_argument('--session', required=True)
    s.add_argument('--output', type=Path, required=True)
    s.add_argument('--bulk-port', type=int, default=1236)
    s.add_argument('--profile-timeout-s', type=int, default=3600)
    s.add_argument('--public-key', type=Path)
    s.add_argument('--boot-set-manifest', type=Path)
    s.add_argument('--repair', action='append', default=[])
    action = s.add_mutually_exclusive_group(required=True)
    action.add_argument('--profile-plan', type=Path)
    action.add_argument('--rollback-profile', type=Path)
    s = sub.add_parser('push')
    s.add_argument('--session', required=True); s.add_argument('--files', type=Path, required=True)
    s.add_argument('--key', type=Path, required=True); s.add_argument('--output', type=Path, required=True)
    s.add_argument('--upload-port', type=int, default=1235, help='receiver port as reachable from here (QEMU: the forward)')
    s.add_argument('--restart', action='store_true'); s.add_argument('--confirm', action='store_true')
    s.add_argument('--boot-timeout-s', type=int, default=180)
    s.add_argument('--tamper', choices=['signature', 'file'], help=argparse.SUPPRESS)
    a = p.parse_args()
    if a.command == 'signer-public':
        print(signing_key(a.key).verify_key.encode().hex())
        return
    from hardware import Hardware
    run = Run(Hardware(), getattr(a, 'output', None))
    if a.command == 'status':
        print(json.dumps(run.status(a.session), indent=2)); return
    if a.command == 'confirm':
        code, text = run.confirm(a.session); print(text); sys.exit(code or 0)
    if a.command == 'profile-recover':
        run.profile_target(a.session)
        run.output.mkdir(parents=True, exist_ok=False)
        _, job = run.job(a.session, ['/boot/bin/bash', '/boot/USBSTAGE.RC', 'recover'])
        run.save(recovery=job)
        sys.exit(job.get('exit', {}).get('code') if job.get('exit') else 1)
    if a.command in ('profile-bootstrap', 'profile'):
        if a.command == 'profile' and a.profile_plan and not (a.public_key and a.boot_set_manifest):
            p.error('--profile-plan requires --public-key and --boot-set-manifest')
        try:
            result = run.profile_bootstrap(a.session) if a.command == 'profile-bootstrap' else run.profile(a)
        except (OSError, RuntimeError, ValueError) as error:
            p.exit(1, f'{error}\nInspect receipts in {a.output}\n')
        print(result['state'])
        return
    if a.command == 'store':
        receipt = run.store(a.session, a.store, a.key, a.first_lba, a.sectors, a.upload_port)
        print(json.dumps({k: receipt.get(k) for k in ('state', 'seconds', 'replies', 'slot_manifest', 'upload_error')}, indent=2))
        sys.exit(0 if receipt.get('state') == 'stored' else 1)
    if a.confirm and not a.restart:
        p.error('--confirm needs --restart (confirm a set only after it booted)')
    try:
        receipt = run.push(a.session, a.files, a.key, a.upload_port, a.restart, a.confirm, a.boot_timeout_s, a.tamper)
    except Exception as error:
        run.save(state='needs_inspection', error=str(error))
        print(json.dumps({'state': 'needs_inspection', 'error': str(error),
                          'receipt': str(a.output / 'receipt.json')}, indent=2))
        sys.exit(1)
    print(json.dumps({k: receipt.get(k) for k in ('state', 'payload_id', 'update', 'new_session', 'new_payload_id',
                                                  'new_status', 'confirm')}, indent=2))
    sys.exit(0 if receipt['state'] in ('updated', 'new_boot_verified', 'confirmed') else 1)


if __name__ == '__main__':
    main()

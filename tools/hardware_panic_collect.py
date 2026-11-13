"""Collect validated, boot-pinned crash snapshots without mutating the guest."""
import hashlib
import json
import os
from pathlib import Path
import stat
import time
import uuid
import zlib


class PanicCollector:
    def __init__(self, bridge, output):
        self.bridge = bridge
        self.output = Path(output).absolute()
        if self.output.is_symlink():
            raise ValueError('Output directory must not be a symlink')
        self.output.mkdir(mode=0o700, parents=True, exist_ok=True)
        if not self.output.is_dir() or self.output.stat().st_uid != os.getuid():
            raise ValueError('Output directory must be owned by this user')

    @staticmethod
    def metadata(reply):
        if reply.get('recovery_panic_error') == 'No validated panic record':
            return None
        seq, size, crc = (reply.get(k) for k in ('sequence', 'bytes', 'crc32'))
        if (reply.get('recovery_panic_api') != 1 or reply.get('offset') != 0 or
                reply.get('hex') != '' or any(type(x) is not int for x in (seq, size, crc)) or
                not 1 <= seq < 2**64 or not 1 <= size <= 65536 or not 0 <= crc <= 0xffffffff):
            raise RuntimeError('Invalid panic snapshot metadata')
        return seq, size, crc

    @staticmethod
    def verify_file(path, size, crc):
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, 'rb') as f:
            info = os.fstat(f.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_size != size:
                raise RuntimeError('Existing panic artifact is not the expected owned file')
            data = f.read(size + 1)
        if len(data) != size or zlib.crc32(data) != crc:
            raise RuntimeError('Existing panic artifact failed its checksum')
        return hashlib.sha256(data).hexdigest()

    def record_observation(self, target, meta, digest, session, info, previous):
        """Record the boot from which the snapshot was read, not the crash boot."""
        boot = info.get('guest_boot_id')
        if not isinstance(boot, str) or len(boot) != 32 or any(c not in '0123456789abcdef' for c in boot):
            raise RuntimeError('Missing pinned observation boot identity')
        seq, size, crc = meta
        fixed = {'observation_boot_id': boot, 'sequence': seq, 'bytes': size,
                 'crc32': f'{crc:08x}', 'sha256': digest, 'artifact': target.name}
        receipt = target.with_name(target.name + '.observed-' + boot + '.json')
        if receipt.exists() or receipt.is_symlink():
            fd = os.open(receipt, os.O_RDONLY | os.O_NOFOLLOW)
            with os.fdopen(fd, 'r') as stream:
                st = os.fstat(stream.fileno())
                if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_size > 4096:
                    raise RuntimeError('Invalid panic observation receipt')
                saved = json.load(stream)
            if not isinstance(saved, dict) or any(saved.get(k) != v for k, v in fixed.items()):
                raise RuntimeError('Panic observation receipt conflicts with artifact')
            return
        data = {**fixed, 'session': session, 'host': info.get('host'), 'port': info.get('port'),
                'journal_slot': 'previous' if previous else 'newest', 'observed_unix': time.time(),
                'boot_identity_scope': 'observation only; crash boot not established'}
        temp = self.output / ('.observation-' + uuid.uuid4().hex + '.tmp')
        try:
            fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'w') as stream:
                json.dump(data, stream, sort_keys=True)
                stream.write('\n')
                stream.flush()
                os.fsync(stream.fileno())
            try:
                os.link(temp, receipt, follow_symlinks=False)
            except FileExistsError:
                self.record_observation(target, meta, digest, session, info, previous)
                return
            directory = os.open(self.output, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            temp.unlink(missing_ok=True)

    def collect(self, session):
        info = self.bridge.load_session(session)
        found = []
        for previous in (False, True):
            command = 'bridge hardware recovery-panic-previous' if previous else 'bridge hardware recovery-panic'
            meta = self.metadata(self.bridge.command(session, command + ' 0 0'))
            if meta is None:
                continue
            seq, size, crc = meta
            target = self.output / f'panic-{seq:020d}-{size}-{crc:08x}.bin'
            if target.exists() or target.is_symlink():
                digest = self.verify_file(target, size, crc)
                self.record_observation(target, meta, digest, session, info, previous)
                found.append({'sequence': seq, 'path': str(target), 'state': 'existing', 'sha256': digest})
                continue
            temp = self.output / ('.panic-' + uuid.uuid4().hex + '.tmp')
            try:
                result = self.bridge.recovery_panic(session, str(temp), previous=previous)
                if (result.get('guest_boot_id') != info.get('guest_boot_id') or
                        result.get('sequence') != seq or result.get('bytes') != size or
                        result.get('crc32') != f'{crc:08x}'):
                    raise RuntimeError('Panic snapshot changed during collection')
                digest = self.verify_file(temp, size, crc)
                if result.get('sha256') != digest:
                    raise RuntimeError('Panic snapshot SHA-256 mismatch')
                fd = os.open(temp, os.O_RDONLY | os.O_NOFOLLOW)
                try:
                    os.fsync(fd)
                finally:
                    os.close(fd)
                try:
                    os.link(temp, target, follow_symlinks=False)
                except FileExistsError:
                    if self.verify_file(target, size, crc) != digest:
                        raise RuntimeError('Conflicting panic artifact already exists')
                dirfd = os.open(self.output, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                try:
                    os.fsync(dirfd)
                finally:
                    os.close(dirfd)
                self.record_observation(target, meta, digest, session, info, previous)
                found.append({'sequence': seq, 'path': str(target), 'state': 'collected', 'sha256': digest})
            finally:
                temp.unlink(missing_ok=True)
        return found

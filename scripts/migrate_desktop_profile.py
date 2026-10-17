#!/usr/bin/env python3
"""Preserve a legacy GIMP profile and install a Brook-owned working copy.

Guest maintenance command; stop GIMP first. Read-only unless --apply is given.
The original inodes stay in a separate backup tree, with their ownership intact.
An interrupted transaction leaves its lock/journal for explicit inspection.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile


def no_links(path):
    path = Path(os.path.abspath(path))
    for component in [*reversed(path.parents), path]:
        if component.is_symlink():
            raise RuntimeError('symlink in maintenance path: %s' % component)
    return path


def inventory(root):
    rows = []

    def visit(path, relative):
        info = path.lstat()
        row = dict(path=relative, mode=stat.S_IMODE(info.st_mode), uid=info.st_uid,
                   gid=info.st_gid, mtime_ns=info.st_mtime_ns)
        if stat.S_ISLNK(info.st_mode):
            row.update(kind='link', target=os.readlink(path))
        elif stat.S_ISREG(info.st_mode):
            with path.open('rb') as file:
                digest = hashlib.file_digest(file, 'sha256').hexdigest()
            row.update(kind='file', size=info.st_size, sha256=digest)
        elif stat.S_ISDIR(info.st_mode):
            row['kind'] = 'directory'
        else:
            raise RuntimeError('unsupported special entry: %s' % path)
        rows.append(row)
        if row['kind'] == 'directory':
            for entry in sorted(path.iterdir()):
                visit(entry, entry.name if relative == '.' else relative+'/'+entry.name)

    visit(root, '.')
    return rows


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)


def write_journal(path, record):
    with (path/'journal.new').open('w') as file:
        json.dump(record, file, indent=2)
        file.write('\n');file.flush();os.fsync(file.fileno())
    os.replace(path/'journal.new', path/'journal.json')
    sync_directory(path)


def copy_profile(source, destination, rows, uid, gid):
    # Originals are preserved by rename, including hardlinks/xattrs/inodes.
    # Build the working copy without following profile symlinks or copying any
    # external symlink target; special files were rejected during inventory.
    for row in rows:
        src, dst = source/row['path'], destination/row['path']
        if row['kind'] == 'directory':
            dst.mkdir(mode=0o700)
        elif row['kind'] == 'link':
            os.symlink(row['target'], dst)
        else:
            with src.open('rb') as original, dst.open('xb') as output:
                shutil.copyfileobj(original, output, 64*1024)
                output.flush();os.fsync(output.fileno())
    for row in reversed(rows):
        dst = destination/row['path']
        os.chown(dst, uid, gid, follow_symlinks=False)
        if row['kind'] != 'link':
            os.chmod(dst, row['mode'])
            os.utime(dst, ns=(row['mtime_ns'], row['mtime_ns']))
            if row['kind'] == 'directory': sync_directory(dst)


def working_matches(rows, actual, uid, gid):
    # Symlink timestamps need not be writable on Brook; link targets and owner
    # still must match. Original timestamps are retained in the backup itself.
    def expected(row):
        result = dict(row, uid=uid, gid=gid)
        if result['kind'] == 'link': result.pop('mtime_ns', None)
        return result
    return [expected(r) for r in rows] == [expected(r) for r in actual] and all(
        r['uid'] == uid and r['gid'] == gid for r in actual)


def migrate(home, uid=1000, gid=100, apply=False):
    home = no_links(home)
    backups = no_links(home.parent/'.brook-profile-backups')
    lock = no_links(backups/'gimp.lock')
    if lock.exists():
        raise RuntimeError('unfinished/concurrent migration; inspect %s' % lock)
    source = no_links(home/'.config/GIMP/3.0')
    if not source.exists(): return dict(status='no-profile', source=str(source))
    if not source.is_dir(): raise RuntimeError('GIMP profile is not a directory')
    parent = source.parent.lstat()
    rows = inventory(source)
    if any(row['uid'] not in (0, uid) for row in rows) or parent.st_uid not in (0, uid):
        raise RuntimeError('profile contains another user; refusing ownership replacement')
    pending = [r['path'] for r in rows if r['uid'] != uid or r['gid'] != gid]
    plan = dict(status='planned', source=str(source), uid=uid, gid=gid,
                entries=len(rows), ownership_changes=len(pending),
                parent_before=dict(uid=parent.st_uid, gid=parent.st_gid, mode=stat.S_IMODE(parent.st_mode)))
    if not pending and parent.st_uid == uid and parent.st_gid == gid:
        return dict(plan, status='already-owned')
    if not apply: return plan
    backups.mkdir(mode=0o700, exist_ok=True)
    info = backups.stat()
    if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise RuntimeError('backup root must be owned by maintenance user with mode 0700')
    if info.st_dev != parent.st_dev: raise RuntimeError('backup must share the profile filesystem')
    sync_directory(backups.parent)
    lock.mkdir(mode=0o700)  # Atomic refusal of concurrent/interrupted migration.
    transaction = Path(tempfile.mkdtemp(prefix='gimp-', dir=backups))
    sync_directory(backups)
    record = dict(plan, status='copying', original=rows, backup=str(transaction/'original'))
    installed = False
    original_moved = False
    try:
        with (lock/'transaction').open('w') as file:
            file.write(str(transaction)+'\n');file.flush();os.fsync(file.fileno())
        sync_directory(lock)
        write_journal(transaction, record)
        staged = transaction/'working'
        copy_profile(source, staged, rows, uid, gid)
        if inventory(source) != rows:
            raise RuntimeError('source changed while copying; stop GIMP and retry')
        if not working_matches(rows, inventory(staged), uid, gid):
            raise RuntimeError('working copy verification failed')
        record['status'] = 'ready';write_journal(transaction, record)
        os.rename(source, transaction/'original')
        original_moved = True
        try:
            sync_directory(source.parent);sync_directory(transaction)
            os.rename(staged, source)
            installed = True
        except BaseException:
            os.rename(transaction/'original', source)
            original_moved = False
            sync_directory(source.parent);sync_directory(transaction)
            raise
        sync_directory(source.parent);sync_directory(transaction)
        # Parent ownership is recorded above; its contents/mode stay intact.
        os.chown(source.parent, uid, gid)
        sync_directory(source.parent)
        if inventory(transaction/'original') != rows:
            raise RuntimeError('preserved original changed; inspect journal')
        record['status'] = 'complete';write_journal(transaction, record)
        return dict(plan, status='complete', backup=str(transaction/'original'),
                    journal=str(transaction/'journal.json'))
    except BaseException as exc:
        record.update(status='failed', error=repr(exc));write_journal(transaction, record)
        raise
    finally:
        # Only normal exception unwinding clears this. A killed process leaves
        # the lock and journal, preventing an automatic second migration.
        if (not installed and not original_moved) or record['status'] == 'complete':
            (lock/'transaction').unlink(missing_ok=True)
            lock.rmdir();sync_directory(backups)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--home', default='/data/home')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    if args.apply and os.geteuid() != 0:
        parser.error('--apply requires root maintenance: run --uid 0 ...')
    if args.apply:
        for entry in Path('/proc').iterdir():
            if not entry.name.isdecimal(): continue
            try: status = (entry/'status').read_text()
            except FileNotFoundError: continue
            name = next((line.split(':', 1)[1].strip() for line in status.splitlines()
                         if line.startswith('Name:')), '').lower().lstrip('.')
            if name.startswith('gimp'):
                parser.error('stop GIMP before applying profile migration')
    # Only the named desktop identity is supported. This is not account setup.
    print(json.dumps(migrate(args.home, apply=args.apply)), flush=True)


if __name__ == '__main__': main()

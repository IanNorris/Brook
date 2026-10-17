#!/usr/bin/env python3
"""Private migration transaction tests on Linux or Brook; never uses real HOME."""
import errno
import json
import os
from pathlib import Path
import sys
import tempfile
import traceback
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import migrate_desktop_profile as migration

uid = 1000 if os.geteuid() == 0 else os.geteuid()
groups = [g for g in os.getgroups() if g != os.getegid()]
mapping = Path('/proc/self/gid_map')
if os.geteuid() != 0 and mapping.exists():
    ranges = [(int(row.split()[0]), int(row.split()[2])) for row in mapping.read_text().splitlines()]
    groups = [g for g in groups if any(start <= g < start+count for start, count in ranges)]
gid = 100 if os.geteuid() == 0 else (groups[0] if groups else os.getegid())
results = {}
parent = sys.argv[2] if len(sys.argv) > 2 else '/tmp'


def fixture():
    root = Path(tempfile.mkdtemp(prefix='brook-profile-test-', dir=parent))
    # Allow the dropped-UID child to traverse to its working copy on Linux too.
    os.chmod(root, 0o755)
    home = root/'home'
    profile = home/'.config/GIMP/3.0'
    profile.mkdir(parents=True)
    (profile/'private').mkdir(mode=0o700)
    (profile/'settings.ini').write_bytes(b'preserve these settings\n')
    os.chmod(profile/'settings.ini', 0o600)
    (profile/'private/state.bin').write_bytes(bytes(range(256))*16)
    (root/'outside').write_bytes(b'outside original')
    (profile/'link').symlink_to('settings.ini')
    (profile/'external-link').symlink_to(root/'outside')
    os.utime(profile/'settings.ini', (1700000000, 1700000000))
    # A freshly created directory can hide a rename timestamp bug if both
    # observations land in one ext2 timestamp tick.
    os.utime(profile, (1600000000, 1600000000))
    return root, home, profile


def invoke(home): return migration.migrate(home, uid, gid, apply=True)


def assert_error(fn):
    try: fn()
    except (OSError, RuntimeError): return
    raise AssertionError('expected refusal/failure')


def remove_fixture(path):
    # These are private flat/recursive test paths. Brook still has a bounded
    # dirfd spelling; use absolute operations so that unrelated limit cannot
    # obscure transaction results or leave the cleanup half-finished.
    if path.is_symlink() or not path.is_dir():
        path.unlink()
    else:
        for entry in list(path.iterdir()): remove_fixture(entry)
        path.rmdir()


def complete(root, home, profile):
    before = migration.inventory(profile)
    inode = profile.stat().st_ino
    outside = (root/'outside').stat()
    planned = migration.migrate(home, uid, gid)
    assert planned['status'] == 'planned', planned
    assert not (root/'.brook-profile-backups').exists()
    result = invoke(home)
    assert result['status'] == 'complete', result
    backup = Path(result['backup'])
    assert backup.stat().st_ino == inode
    assert migration.inventory(backup) == before
    assert migration.working_matches(before, migration.inventory(profile), uid, gid)
    assert (root/'outside').stat().st_uid == outside.st_uid
    assert (root/'outside').stat().st_gid == outside.st_gid
    assert (root/'outside').read_bytes() == b'outside original'
    assert invoke(home)['status'] == 'already-owned'
    assert len(list((root/'.brook-profile-backups').glob('gimp-*'))) == 1
    assert json.loads(Path(result['journal']).read_text())['status'] == 'complete'
    if os.geteuid() == 0:
        pid = os.fork()
        if pid == 0:
            try:
                os.setgroups([]);os.setresgid(gid, gid, gid);os.setresuid(uid, uid, uid)
                with (profile/'settings.ini').open('ab') as f:
                    f.write(b'Brook can update\n');f.flush();os.fsync(f.fileno())
                assert (profile/'private/state.bin').read_bytes() == bytes(range(256))*16
                os._exit(0)
            except BaseException: os._exit(1)
        assert os.waitpid(pid, 0)[1] == 0
    return 'original inodes/metadata/content retained; owned copy; symlinks; idempotence'


def rollback(root, home, profile):
    before = migration.inventory(profile);inode = profile.stat().st_ino
    rename = os.rename
    hit = []
    def fail_working(src, dst, *args, **kwargs):
        if Path(src).name == 'working':
            hit.append(True);raise OSError(errno.EIO, 'injected swap failure')
        return rename(src, dst, *args, **kwargs)
    with patch.object(migration.os, 'rename', fail_working): assert_error(lambda: invoke(home))
    assert hit, 'did not reach swap fault injection'
    assert profile.stat().st_ino == inode and migration.inventory(profile) == before
    assert not (root/'.brook-profile-backups/gimp.lock').exists()
    return 'failed cutover restores original path/inodes/ownership'


def copy_failure(root, home, profile):
    before = migration.inventory(profile);inode = profile.stat().st_ino
    with patch.object(migration.shutil, 'copyfileobj', side_effect=OSError(errno.ENOSPC, 'injected copy failure')) as fault:
        assert_error(lambda: invoke(home))
    assert fault.called, 'did not reach copy fault injection'
    assert profile.stat().st_ino == inode and migration.inventory(profile) == before
    assert not (root/'.brook-profile-backups/gimp.lock').exists()
    return 'failed copy leaves original untouched'


def interrupted(root, home, profile):
    before = migration.inventory(profile);inode = profile.stat().st_ino
    pid = os.fork()
    if pid == 0:
        rename = os.rename
        def cut_power(src, dst, *args, **kwargs):
            result = rename(src, dst, *args, **kwargs)
            if Path(src) == profile: os._exit(77)
            return result
        try:
            with patch.object(migration.os, 'rename', cut_power): invoke(home)
        except BaseException:
            traceback.print_exc()
        os._exit(1)
    assert os.waitpid(pid, 0)[1] == 77 << 8
    assert not profile.exists()
    assert_error(lambda: invoke(home))
    transaction = Path((root/'.brook-profile-backups/gimp.lock/transaction').read_text().strip())
    original = transaction/'original'
    assert original.stat().st_ino == inode and migration.inventory(original) == before
    # Explicit recovery is confined to this test's private fixture.
    os.rename(original, profile)
    return 'interrupted cutover preserves original and refuses automatic re-entry'


def conflicts(root, home, profile):
    inode = profile.stat().st_ino
    if os.geteuid() == 0:
        os.chown(profile/'settings.ini', 1001, 100)
        assert_error(lambda: invoke(home))
        assert (profile/'settings.ini').stat().st_uid == 1001
    moved = profile.with_name('saved');profile.rename(moved);profile.symlink_to(moved)
    assert_error(lambda: invoke(home))
    assert moved.stat().st_ino == inode and profile.is_symlink()
    return 'symlink profile refused; foreign UID also checked when root'


for name, fn in [('complete', complete), ('rollback', rollback), ('copy-failure', copy_failure),
                 ('interrupted', interrupted), ('conflicts', conflicts)]:
    root, home, profile = fixture()
    try:
        if os.geteuid() != 0 and gid == os.getegid() and name != 'conflicts':
            results[name] = dict(skipped='host needs an alternate supplementary group')
        else:
            results[name] = dict(passed=True, detail=fn(root, home, profile))
    except BaseException as exc:
        traceback.print_exc()
        results[name] = dict(passed=False, error=repr(exc))
    finally:
        remove_fixture(root)
    print('MIGRATION_CASE', name, json.dumps(results[name]), flush=True)
if len(sys.argv)>1: Path(sys.argv[1]).write_text(json.dumps(results, indent=2)+'\n')
print('MIGRATION_RESULT', json.dumps(results), flush=True)
if any(r.get('passed') is False for r in results.values()): raise SystemExit(1)

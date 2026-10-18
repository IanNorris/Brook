#!/usr/bin/env python3
"""Private parent/sticky mutation matrix. Root guest; not traversal acceptance."""
import argparse
import errno
import json
import os
from pathlib import Path
import re
import stat

A, B, C, GROUP = 66536, 66537, 66538, 131172
PAYLOAD = b'preserve denied mutation\n'


def child(uid, gid, groups, operation):
    read, write = os.pipe(); pid = os.fork()
    if pid == 0:
        os.close(read)
        try:
            os.setgroups(groups); os.setgid(gid); os.setuid(uid)
            try:
                operation(); error = 0
            except OSError as e:
                error = e.errno
            os.write(write, str(error).encode()); os._exit(0)
        except BaseException:
            os._exit(1)
    os.close(write); data = b''
    while chunk := os.read(read, 4096): data += chunk
    os.close(read)
    assert os.waitpid(pid, 0) == (pid, 0), 'child failed'
    return int(data)


def snapshot(parents):
    rows = []
    for parent in parents:
        rows.append((str(parent), int(parent.stat().st_mtime)))
        for path in sorted(parent.iterdir()):
            s = path.lstat()
            value = os.readlink(path) if stat.S_ISLNK(s.st_mode) else path.read_bytes().hex() if stat.S_ISREG(s.st_mode) else 'directory'
            rows.append((str(path), s.st_ino, s.st_uid, s.st_gid, s.st_mode, s.st_nlink, value))
    return rows


def fixture(root, name, mode, sticky=False, uid=A):
    directory = root/name; directory.mkdir()
    parents = [directory/'left', directory/'right']
    for p in parents:
        p.mkdir(); os.chown(p, A, GROUP); p.chmod(mode)
    for p in [parents[0]/'file', parents[1]/'victim']:
        p.write_bytes(PAYLOAD); os.chown(p, B if p.name == 'file' else C, GROUP); p.chmod(0)
    (parents[0]/'empty').mkdir(); os.chown(parents[0]/'empty', B, GROUP)
    # The symlink entry's ownership, not its referent's, controls sticky removal.
    os.symlink('../right/victim', parents[0]/'link')
    os.lchown(parents[0]/'link', uid if sticky else B, GROUP)
    for p in parents: os.utime(p, (1000000000, 1000000000))
    return parents


def operation(name, left, right):
    def create():
        fd = os.open(left/'created', os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.write(fd, PAYLOAD); os.close(fd)
    return {
        'create': create,
        'mkdir': lambda: os.mkdir(left/'created-dir'),
        'symlink': lambda: os.symlink('file', left/'created-link'),
        'unlink': lambda: os.unlink(left/'file'),
        'rmdir': lambda: os.rmdir(left/'empty'),
        'rename-source': lambda: os.rename(left/'file', right/'moved'),
        'rename-dest': lambda: os.rename(right/'victim', left/'moved'),
        'rename-replace': lambda: os.rename(left/'file', right/'victim'),
        'unlink-link': lambda: os.unlink(left/'link'),
    }[name]


def cleanup(directory):
    # Brook's deep directory FDs still retain a shortened path. Keep fixture
    # cleanup absolute; this probe does not accept that independent dirfd gap.
    for path in directory.iterdir():
        if path.is_dir() and not path.is_symlink():
            cleanup(path)
        else:
            path.unlink()
    directory.rmdir()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', default='/data'); ap.add_argument('--case', required=True)
    ap.add_argument('--record-baseline', action='store_true')
    args = ap.parse_args(); assert os.geteuid() == 0 and re.fullmatch('[0-9a-f]{16}', args.case)
    root = Path(args.root)/('brook-mutation-'+args.case); root.mkdir(mode=0o755); root.chmod(0o755)
    rows = []
    def check(name, actual, expected):
        row = dict(case=name, actual=actual, expected=expected, matches=actual == expected)
        rows.append(row); print(json.dumps(row), flush=True)
    try:
        actors = [('root',0,0,[],0,True), ('owner-no-write',A,100,[],0o030,False),
                  ('primary',B,GROUP,[],0o030,True), ('supplementary',B,100,[GROUP],0o030,True),
                  ('other',B,100,[],0o003,True), ('group-no-fallback',B,GROUP,[],0o003,False),
                  ('no-search',B,100,[],0o002,False), ('no-write',B,100,[],0o001,False)]
        names = ['create','mkdir','symlink','unlink','rmdir','rename-source','rename-dest','rename-replace','unlink-link']
        for label, uid, gid, groups, mode, allowed in actors:
            for name in names:
                parents = fixture(root, label+'-'+name, mode)
                # Exercise each rename parent independently.
                if name == 'rename-source': parents[1].chmod(0o777)
                if name == 'rename-dest': parents[1].chmod(0o777)
                before = snapshot(parents)
                actual = child(uid,gid,groups,lambda: operation(name,*parents)())
                check(label+'-'+name, actual, 0 if allowed else errno.EACCES)
                if not allowed and actual != 0:
                    check(label+'-'+name+'-preserved',snapshot(parents)==before,True)
        for label, uid in [('root',0),('parent-owner',A),('source-owner',B),('victim-owner',C),('unrelated',C+1),('low16-parent-collision',1000),('low16-source-collision',1001)]:
            for name in ['unlink','rmdir','rename-source','rename-dest','rename-replace','unlink-link']:
                parents = fixture(root,'sticky-'+label+'-'+name,0o1777,sticky=True,uid=B)
                source_owner = C if name == 'rename-dest' else B
                allowed = uid in (0,A) or uid == source_owner and name != 'rename-replace'
                before = snapshot(parents);actual = child(uid,100,[],lambda: operation(name,*parents)())
                check('sticky-'+label+'-'+name, actual, 0 if allowed else errno.EPERM)
                if not allowed and actual != 0:
                    check('sticky-'+label+'-'+name+'-preserved',snapshot(parents)==before,True)
        # A sticky symlink cannot borrow its target's owner permission.
        parents=fixture(root,'sticky-referent-owner',0o1777,sticky=True,uid=B)
        check('sticky-referent-owner',child(C,100,[],lambda: os.unlink(parents[0]/'link')),errno.EPERM)
        for label, uid, gid, groups, owner, mode, cross, allowed in [
            ('same-parent-no-child-write',B,100,[],A,0,False,True),
            ('cross-parent-no-child-write',B,100,[],A,0,True,False),
            ('cross-parent-owner-write',B,100,[],B,0o200,True,True),
            ('cross-parent-supplementary-write',B,100,[GROUP],A,0o020,True,True),
            ('cross-parent-group-no-fallback',B,GROUP,[],A,0o002,True,False),
            ('cross-parent-root',0,0,[],A,0,True,True),
        ]:
            parents=fixture(root,label,0o777)
            source=parents[0]/'empty';os.chown(source,owner,GROUP);source.chmod(mode)
            destination=(parents[1] if cross else parents[0])/'moved-dir'
            before=snapshot(parents)
            actual=child(uid,gid,groups,lambda: os.rename(source,destination))
            check(label,actual,0 if allowed else errno.EACCES)
            if not allowed and actual != 0:check(label+'-preserved',snapshot(parents)==before,True)
        # O_CREAT on an existing writable file needs no parent write access.
        parents=fixture(root,'existing-file-readonly-parent',0o555)
        path=parents[0]/'file';path.chmod(0o666)
        def open_existing():
            fd=os.open(path,os.O_CREAT|os.O_RDWR,0o600);os.close(fd)
        check('existing-file-readonly-parent',child(B,100,[],open_existing),0)
    finally:
        cleanup(root); os.sync()
    failures=[x['case'] for x in rows if not x['matches']]
    print('DIRECTORY_MUTATION_FAILURES',json.dumps(failures),flush=True)
    assert bool(failures) if args.record_baseline else not failures, failures
    print('DIRECTORY_MUTATION_BASELINE_RECORDED' if args.record_baseline else 'DIRECTORY_MUTATIONS_PASS',flush=True)


if __name__ == '__main__': main()

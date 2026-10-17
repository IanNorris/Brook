#!/usr/bin/env python3
"""Credential acceptance in private child processes; parent identity is unchanged.

Run as root in Brook for privileged cases. Host Linux can run the nonprivileged
query/buffer checks. Optional args: JSON output path, then 'brook' for explicit
unsupported-capability expectations. Uses only the Python standard library.
"""
import ctypes
import errno
import json
import os
import pwd
import grp
import struct
import sys
import threading

libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long
libc.getauxval.restype = ctypes.c_ulong
results = {}

def raw(number, *args):
    ctypes.set_errno(0)
    value = libc.syscall(ctypes.c_long(number), *(ctypes.c_void_p(x) for x in args))
    if value == -1:
        raise OSError(ctypes.get_errno(), 'syscall %d' % number)
    return value

def denied(error, fn):
    try:
        fn()
    except OSError as e:
        assert e.errno == error, (e.errno, error)
    else:
        raise AssertionError('expected errno %d' % error)

def snapshot():
    return {'uid': list(os.getresuid()), 'gid': list(os.getresgid()), 'groups': os.getgroups()}

def child_case(name, fn, privileged=True):
    if privileged and os.geteuid() != 0:
        results[name] = {'skipped': 'requires root in the guest'}
        return
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(r)
        try:
            detail = fn(w)
            payload = {'passed': True, 'detail': detail}
        except BaseException as e:
            payload = {'passed': False, 'error': repr(e), 'identity': snapshot()}
        os.write(w, json.dumps(payload).encode())
        os.close(w)
        os._exit(0)
    os.close(w)
    data = bytearray()
    while chunk := os.read(r, 65536):
        data.extend(chunk)
    os.close(r)
    _, status = os.waitpid(pid, 0)
    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, (name, status)
    result = json.loads(data)
    results[name] = result
    print('IDENTITY_CASE', name, json.dumps(result), flush=True)
    assert result.get('passed'), (name, result)

def queries(_):
    assert os.getresuid()[:2] == (os.getuid(), os.geteuid())
    assert os.getresgid()[:2] == (os.getgid(), os.getegid())
    # Auxv reports initial launch/exec credentials, not subsequent transitions.
    aux = [libc.getauxval(x) for x in (11, 12, 13, 14)]
    assert aux == [os.getuid(), os.geteuid(), os.getgid(), os.getegid()], aux
    return {'identity': snapshot(), 'auxv': aux}

def launch_metadata(_):
    for path in ('/etc/passwd', '/etc/group'):
        with open(path, 'rb') as f:
            content = f.read()
            assert len(content) == os.stat(path).st_size == os.lstat(path).st_size == os.fstat(f.fileno()).st_size
            f.seek(0)
            assert f.read(os.stat(path).st_size) == content
        assert content.endswith(b'\n'), path
    desktop = pwd.getpwnam('brook')
    assert (desktop.pw_uid, desktop.pw_gid, desktop.pw_dir) == (1000, 100, '/data/home'), desktop
    assert 'brook' in grp.getgrgid(100).gr_mem
    account = pwd.getpwuid(os.getuid())
    assert os.environ['USER'] == os.environ['LOGNAME'] == account.pw_name
    assert os.environ['HOME'] == account.pw_dir == '/data/home'
    assert os.getgid() == account.pw_gid
    return {'name': account.pw_name, 'uid': account.pw_uid, 'gid': account.pw_gid, 'home': account.pw_dir}

def buffers(_):
    target = (ctypes.c_uint32 * 3)(91, 92, 93)
    ptr = ctypes.addressof(target)
    for number in (118, 120):
        for missing in range(3):
            args = [ptr, ptr+4, ptr+8]; args[missing] = 0
            denied(errno.EFAULT, lambda: raw(number, *args))
    # Capacity is a signed int in the syscall ABI.
    denied(errno.EINVAL, lambda: raw(115, 0xffffffff, ptr))
    return 'required result pointers and signed capacity validation'

def thread_signals(_):
    mask = ctypes.c_uint64(1 << (10 - 1))
    old = ctypes.c_uint64()
    raw(14, 0, ctypes.addressof(mask), ctypes.addressof(old), 8)
    try:
        tid = raw(186)
        denied(errno.ESRCH, lambda: raw(234, os.getpid() + 65536, tid, 0))
        denied(errno.ESRCH, lambda: raw(234, os.getpid() + 1, tid, 0))
        denied(errno.ESRCH, lambda: raw(234, os.getpid(), tid + 65536, 0))
        denied(errno.EINVAL, lambda: raw(234, os.getpid(), tid, 65))
        raw(234, os.getpid(), tid, 0)
        raw(234, os.getpid(), tid, 10)
        info = ctypes.create_string_buffer(128)
        timeout = (ctypes.c_int64 * 2)(0, 0)
        assert raw(128, ctypes.addressof(mask), ctypes.addressof(info), ctypes.addressof(timeout), 8) == 10
        signo, _, code, _, sender, uid = struct.unpack_from('<iiiiII', info.raw)
        assert (signo, code, sender, uid) == (10, -6, os.getpid(), os.getuid()), (signo, code, sender, uid)
        denied(errno.EAGAIN, lambda: raw(128, ctypes.addressof(mask), 0, ctypes.addressof(timeout), 8))
    finally:
        raw(14, 2, ctypes.addressof(old), 0, 8)
    return 'tgkill validates thread group/TID and preserves SI_TKILL sender metadata'

def clone_bad_parent_tid(_):
    # Valid pthread sharing flags, but no writable parent-TID result. The
    # failure must happen before an unpublished child can retain resources.
    flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x100000
    for address in (0, 1):
        for _ in range(16):
            denied(errno.EFAULT, lambda: raw(56, flags, 0, address, 0, 0))
    return '32 failed thread clones reject bad parent-TID pointers'

def temporary_uid(_):
    os.setresuid(1000, 1000, 0)
    assert os.getresuid() == (1000, 1000, 0)
    os.setuid(0)
    assert os.getresuid() == (1000, 0, 0)
    os.setuid(2000)
    assert os.getresuid() == (2000, 2000, 2000)
    denied(errno.EPERM, lambda: os.setuid(0))
    denied(errno.EINVAL, lambda: raw(105, 0xffffffff))
    assert os.getresuid() == (2000, 2000, 2000)
    return 'temporary effective drop/regain and irreversible three-ID drop'

def re_ids(_):
    os.setresgid(100, 200, 300)
    os.setresuid(1000, 2000, 3000)
    os.setresuid(3000, 1000, 2000)
    assert os.getresuid() == (3000, 1000, 2000)
    denied(errno.EPERM, lambda: os.setresuid(1000, 9999, 3000))
    assert os.getresuid() == (3000, 1000, 2000)
    os.setreuid(-1, 3000)
    assert os.getresuid() == (3000, 3000, 2000)
    os.setreuid(-1, 2000)
    assert os.getresuid() == (3000, 2000, 2000)
    os.setgid(300)
    assert os.getresgid() == (100, 300, 300)
    os.setregid(300, 100)
    assert os.getresgid() == (300, 100, 100)
    denied(errno.EPERM, lambda: os.setregid(9999, 300))
    assert os.getresgid() == (300, 100, 100)
    return 'old-tuple validation and saved-ID rules for UID/GID'

def groups(_):
    os.setgroups([9, 4, 9, 4294967294])
    assert os.getgroups() == [4, 9, 9, 4294967294]
    target = (ctypes.c_uint32 * 4)()
    ptr = ctypes.addressof(target)
    assert raw(115, 0, 1) == 4
    denied(errno.EINVAL, lambda: raw(115, 2, ptr))
    denied(errno.EFAULT, lambda: raw(115, 4, 1))
    denied(errno.EFAULT, lambda: raw(116, 1, 1))
    invalid = ctypes.c_uint32(0xffffffff)
    denied(errno.EINVAL, lambda: raw(116, 1, ctypes.addressof(invalid)))
    denied(errno.EINVAL, lambda: raw(116, 65537, ptr))
    assert os.getgroups() == [4, 9, 9, 4294967294]
    os.setgroups([i % 97 for i in range(65536)])
    large = os.getgroups()
    assert len(large) == 65536 and large == sorted(large)
    os.setgroups([]); assert os.getgroups() == []
    os.setuid(1000)
    denied(errno.EPERM, lambda: os.setgroups([]))
    return 'duplicates/high IDs, 65536 entries, buffers, failed replacement and denied clearing'

def group_inheritance(_):
    os.setgroups([11, 22])
    r, w = os.pipe(); pid = os.fork()
    if pid == 0:
        os.close(r)
        before = os.getgroups(); os.setgroups([33]); after = os.getgroups()
        os.write(w, json.dumps([before, after]).encode()); os._exit(0)
    os.close(w); child = json.loads(os.read(r, 1000)); os.close(r)
    _, status = os.waitpid(pid, 0)
    assert status == 0 and child == [[11, 22], [33]], (status, child)
    assert os.getgroups() == [11, 22]
    observed = []
    def change_thread_only():
        observed.append(os.getgroups())
        group = ctypes.c_uint32(77)
        raw(116, 1, ctypes.addressof(group))
        observed.append(os.getgroups())
    t = threading.Thread(target=change_thread_only); t.start(); t.join(3)
    assert not t.is_alive() and observed == [[11,22],[77]], observed
    assert os.getgroups() == [11,22]
    return 'fork and raw per-thread independent list replacement'

def libc_threads(_):
    # libc, unlike raw syscalls, coordinates changes across a thread group.
    os.setgroups([3])
    ready = threading.Event(); finish = threading.Event(); seen = []
    def peer():
        ready.set(); finish.wait(3)
        seen.append((os.getresuid(), os.getgroups()))
    t = threading.Thread(target=peer); t.start(); assert ready.wait(2)
    os.setgroups([5, 7])
    os.setuid(1000)
    finish.set(); t.join(3)
    assert not t.is_alive() and seen == [((1000,1000,1000),[5,7])], seen
    assert os.getresuid() == (1000,1000,1000) and os.getgroups() == [5,7]
    return 'glibc setgroups/setuid coordinate across pthreads'

def raw_uid_thread(_):
    seen = []
    def peer():
        raw(117, 1000, 1000, 1000)
        seen.append(os.getresuid())
    t = threading.Thread(target=peer); t.start(); t.join(3)
    assert not t.is_alive() and seen == [(1000,1000,1000)], seen
    assert os.getresuid() == (0,0,0)
    return 'raw setresuid changes the calling thread only'

def exec_case(w, mismatch=False):
    os.setgroups([13, 17])
    os.setresgid(1000, 1000, 0)
    os.setresuid(1000, 2000 if mismatch else 1000, 0)
    os.set_inheritable(w, True)
    expected_euid = 2000 if mismatch else 1000
    program = r'''
import os,ctypes,json,sys
c=ctypes.CDLL(None);c.getauxval.restype=ctypes.c_ulong
u=os.getresuid();g=os.getresgid();a=[c.getauxval(i) for i in (11,12,13,14,23)]
assert u==(1000,EXPECTED,EXPECTED),u
assert g==(1000,1000,1000),g
assert os.getgroups()==[13,17]
assert a==[1000,EXPECTED,1000,1000,SECURE],a
os.write(FD,json.dumps({'passed':True,'detail':{'uid':u,'gid':g,'auxv':a,'groups':os.getgroups()}}).encode())
'''.replace('EXPECTED',str(expected_euid)).replace('SECURE','1' if mismatch else '0').replace('FD',str(w))
    os.execv(sys.executable, [sys.executable, '-c', program])

def capability_errors(_):
    header = (ctypes.c_uint32 * 2)(0x20080522, 0)
    data = (ctypes.c_uint32 * 6)(*[0xdeadbeef]*6)
    for number in (125, 126):
        denied(errno.ENOSYS, lambda: raw(number, ctypes.addressof(header), ctypes.addressof(data)))
        assert list(data) == [0xdeadbeef]*6
    for option in (7,8,21,22,23,24,27,28,38,39,47):
        denied(errno.EINVAL, lambda: raw(157, option, 0, 0, 0, 0))
    return 'unsupported capability/security controls fail explicitly without writing buffers'

try:
    child_case('queries', queries, False)
    if len(sys.argv) > 2 and sys.argv[2] == 'brook':
        child_case('launch_metadata', launch_metadata, False)
    child_case('buffers', buffers, False)
    child_case('thread_signals', thread_signals, False)
    if len(sys.argv) > 2 and sys.argv[2] == 'brook':
        child_case('clone_bad_parent_tid', clone_bad_parent_tid, False)
    child_case('temporary_uid', temporary_uid)
    child_case('re_ids', re_ids)
    child_case('groups', groups)
    child_case('group_inheritance', group_inheritance)
    child_case('raw_uid_thread', raw_uid_thread)
    child_case('libc_threads', libc_threads)
    child_case('exec', exec_case)
    child_case('secure_exec', lambda w: exec_case(w, True))
    if len(sys.argv) > 2 and sys.argv[2] == 'brook':
        child_case('capabilities_unsupported', capability_errors, False)
finally:
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'w') as f: json.dump(results, f, indent=2)
    print('IDENTITY_RESULT', json.dumps(results), flush=True)

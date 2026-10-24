#!/usr/bin/env python3
"""Verify resolver read/metadata identity and optional isolated bind-mount use.

Run --bind as root inside a disposable Brook VM. The child changes only its
own mount namespace and removes its scratch files before exiting.
"""
import argparse
import ctypes as c
import ipaddress
import json
import os
from pathlib import Path
import signal
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', action='store_true')
    args = parser.parse_args()
    path = Path('/etc/resolv.conf')
    data = path.read_bytes()
    addresses = [line.split()[1].decode() for line in data.splitlines()
                 if line.split() and line.split()[0] == b'nameserver']
    assert addresses, 'resolver has no nameserver'
    for address in addresses:
        ipaddress.ip_address(address)
    stat = path.stat()
    cases = dict(nonempty_resolver=True)
    for name, flags in [('read', os.O_RDONLY), ('path', os.O_PATH)]:
        fd = os.open(path, flags)
        try:
            value = os.fstat(fd)
            cases[name + '_identity'] = (value.st_dev, value.st_ino) == (stat.st_dev, stat.st_ino)
            cases[name + '_size'] = value.st_size == len(data)
        finally:
            os.close(fd)
    cases['stat_size'] = stat.st_size == len(data)
    print('RESOLVER_FILE_RESULT', json.dumps(dict(passed=all(cases.values()), cases=cases)), flush=True)
    assert all(cases.values())
    if not args.bind:
        return
    assert os.geteuid() == 0, '--bind requires root in a disposable guest'
    signal.alarm(30)
    lib = c.CDLL(None, use_errno=True)
    lib.syscall.restype = c.c_long
    lib.syscall.argtypes = [c.c_long] * 7
    lib.mount.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.c_ulong, c.c_void_p]
    lib.umount2.argtypes = [c.c_char_p, c.c_int]
    child = lib.syscall(56, 0x20000 | 17, 0, 0, 0, 0, 0)
    assert child >= 0, c.get_errno()
    if child:
        _, status = os.waitpid(child, 0)
        assert status == 0, status
        print('RESOLVER_BIND_RESULT', json.dumps(dict(passed=True)), flush=True)
        return
    signal.alarm(30)  # fork-style clone does not inherit the parent's alarm
    assert lib.mount(None, b'/', None, 0x44000, None) == 0, c.get_errno()
    with tempfile.TemporaryDirectory(prefix='resolver-file-') as directory:
        target = Path(directory) / 'bound'
        target.touch()
        assert lib.mount(bytes(path), bytes(target), None, 4096, None) == 0, c.get_errno()
        try:
            assert target.read_bytes() == data
            other = target.stat()
            assert (other.st_dev, other.st_ino) == (stat.st_dev, stat.st_ino)
        finally:
            assert lib.umount2(bytes(target), 2) == 0, c.get_errno()
    os._exit(0)


if __name__ == '__main__':
    main()

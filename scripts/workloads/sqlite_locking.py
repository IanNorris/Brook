#!/usr/bin/env python3
"""Fresh-file GETLK/SQLite diagnostic, not multi-writer exclusion acceptance.

Run with Python's sqlite3 module on x86-64 Linux and Brook. Pass an ext2 directory
in Brook: FAT has separate open/unlink/rmdir restrictions. Only private temporary
files are created. SQLite's WAL protocol retry may exceed its busy timeout.
"""
import argparse
import ctypes
import json
from pathlib import Path
import sqlite3
import tempfile


class Flock(ctypes.Structure):
    _fields_ = [('l_type', ctypes.c_short), ('l_whence', ctypes.c_short),
                ('l_start', ctypes.c_longlong), ('l_len', ctypes.c_longlong),
                ('l_pid', ctypes.c_int)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', required=True)
    args = parser.parse_args()
    if ctypes.sizeof(Flock) != 32 or Flock.l_start.offset != 8:
        parser.error('requires Linux x86-64 flock ABI')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.fcntl.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    results = {}
    def emit():
        print(json.dumps({'sqlite_version': sqlite3.sqlite_version, 'results': results}), flush=True)
    with tempfile.TemporaryDirectory(prefix='brook-locking-', dir=args.directory) as tmp:
        with (Path(tmp) / 'unlocked').open('w+b') as file:
            query = Flock(1, 0, 0x40000001, 1, 0)  # WRLCK, SEEK_SET
            ctypes.set_errno(0)
            result = libc.fcntl(file.fileno(), 5, ctypes.byref(query))  # GETLK
            results['getlk'] = {'passed': result == 0 and query.l_type == 2,
                                'result': result, 'errno': ctypes.get_errno(),
                                'reported_type': query.l_type, 'expected_type': 2}
        emit()
        for mode in ('DELETE', 'WAL'):
            connection = None
            try:
                connection = sqlite3.connect(str(Path(tmp) / (mode + '.db')), timeout=.2)
                actual = connection.execute('PRAGMA journal_mode=' + mode).fetchone()[0]
                connection.execute('CREATE TABLE example (value TEXT)')
                connection.execute("INSERT INTO example VALUES ('brook')")
                connection.commit()
                value = connection.execute('SELECT value FROM example').fetchone()[0]
                results[mode] = {'passed': value == 'brook' and actual.upper() == mode,
                                 'journal_mode': actual}
            except sqlite3.Error as error:
                results[mode] = {'passed': False, 'error': str(error),
                                 'sqlite_errorcode': getattr(error, 'sqlite_errorcode', None),
                                 'sqlite_errorname': getattr(error, 'sqlite_errorname', None)}
            finally:
                if connection is not None:
                    connection.close()
            emit()
    return 0 if all(result['passed'] for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

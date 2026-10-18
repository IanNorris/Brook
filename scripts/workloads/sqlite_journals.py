#!/usr/bin/env python3
"""Synthetic SQLite journal/read-only reopen probe; never opens user databases."""
import json
from contextlib import closing
import sqlite3
import sys
import tempfile
from pathlib import Path


def run_case(directory, mode):
    path = directory / (mode.lower() + '.sqlite')
    stage = 'create'
    try:
        with closing(sqlite3.connect(path)) as writer:
            actual = writer.execute('pragma journal_mode=' + mode).fetchone()[0]
            assert actual.lower() == mode.lower(), (mode, actual)
            writer.execute('create table samples (id integer primary key, value text)')
            writer.executemany('insert into samples values (?,?)', [(i, 'sample-' + str(i)) for i in range(100)])
            writer.commit()
            stage = 'readonly-concurrent'
            reader = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=1)
            try:
                assert reader.execute('select count(*),sum(id) from samples').fetchone() == (100, 4950)
            finally:
                reader.close()
            if mode == 'WAL':
                stage = 'checkpoint'
                assert writer.execute('pragma wal_checkpoint(truncate)').fetchone()[0] == 0
        stage = 'readonly-reopen'
        reader = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=1)
        try:
            assert reader.execute('select count(*),sum(id) from samples').fetchone() == (100, 4950)
            assert reader.execute('pragma quick_check').fetchone() == ('ok',)
        finally:
            reader.close()
        return {'mode': mode, 'pass': True}
    except Exception as error:
        return {'mode': mode, 'pass': False, 'stage': stage, 'error': str(error),
                'sqlite_errorcode': getattr(error, 'sqlite_errorcode', None),
                'sqlite_errorname': getattr(error, 'sqlite_errorname', None)}


def main():
    assert len(sys.argv) == 2, 'usage: sqlite_journals.py WRITABLE_DIRECTORY'
    with tempfile.TemporaryDirectory(prefix='brook-sqlite-journals-', dir=sys.argv[1]) as temporary:
        cases = [run_case(Path(temporary).resolve(), mode) for mode in ('DELETE', 'TRUNCATE', 'PERSIST', 'WAL')]
        print(json.dumps({'sqlite_version': sqlite3.sqlite_version, 'cases': cases}), flush=True)
        return 0 if all(case['pass'] for case in cases) else 1


if __name__ == '__main__':
    sys.exit(main())

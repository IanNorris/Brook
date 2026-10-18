#!/usr/bin/env python3
"""Bounded, owned download/decompression controls with matched tool identities."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(root, cat, zstd, curl, url, repeats):
    root = Path(root).resolve(strict=True)
    output = root / 'stage-results'
    output.mkdir(exist_ok=False)
    manifest = json.loads((root / 'manifest.json').read_text())['shapes']['bulk']
    for suffix, key in (('.nar', 'nar_sha256'), ('.nar.zst', 'zst_sha256')):
        assert sha(root / ('bulk' + suffix)) == manifest[key]
    tools = {name: dict(path=str(Path(path).resolve()), sha256=sha(Path(path).resolve()))
             for name, path in (('cat', cat), ('zstd', zstd), ('curl', curl))}
    print('STAGE_TOOLS', json.dumps(tools), flush=True)
    raw = str(root / 'bulk.nar')
    compressed = raw + '.zst'
    http = lambda name: [curl, '-fsS', url.rstrip('/') + '/' + name]
    cases = [
        ('cat-file', [[cat, raw]], manifest['nar_sha256']),
        ('zstd-file', [[zstd, '-dc', compressed]], manifest['nar_sha256']),
        ('zstd-cat-file', [[zstd, '-dc', compressed], [cat]], manifest['nar_sha256']),
        ('http-raw-file', [http('bulk.nar')], manifest['nar_sha256']),
        ('http-zst-file', [http('bulk.nar.zst')], manifest['zst_sha256']),
        ('http-zstd-file', [http('bulk.nar.zst'), [zstd, '-dc']], manifest['nar_sha256']),
        ('http-zstd-cat-file', [http('bulk.nar.zst'), [zstd, '-dc'], [cat]], manifest['nar_sha256']),
    ]
    rows = []
    for name, commands, expected in cases:
        print('STAGE_BEGIN', name, flush=True)
        for index in range(repeats):
            destination = output / (name + '.out')
            children = []
            logs = []
            previous = None
            start = time.monotonic_ns()
            try:
                with destination.open('wb') as sink:
                    for i, argv in enumerate(commands):
                        log = (output / f'{name}-{index}-{i}.stderr').open('wb')
                        logs.append(log)
                        child = subprocess.Popen(argv, stdin=previous or subprocess.DEVNULL,
                            stdout=sink if i + 1 == len(commands) else subprocess.PIPE,
                            stderr=log)
                        children.append(child)
                        if previous is not None:
                            previous.close()
                        previous = child.stdout
                    statuses = [child.wait() for child in reversed(children)][::-1]
                elapsed = time.monotonic_ns() - start
                assert not any(statuses), statuses
            finally:
                if previous is not None:
                    previous.close()
                for child in children:
                    if child.poll() is None:
                        child.terminate()
                for child in children:
                    child.wait()
                for log in logs:
                    log.close()
            assert sha(destination) == expected, name
            row = dict(case=name, repeat=index, elapsed_ns=elapsed,
                       statuses=statuses, verified=True)
            rows.append(row)
            print('STAGE_RESULT', json.dumps(row), flush=True)
        fd = os.open(destination, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    report = dict(uid=os.getuid(), tools=tools, rows=rows, complete=True,
                  scope='Warmed synthetic inputs; extraction timing includes child startup and output writes; hash verification and final fsync excluded. Last output per case retained.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('STAGE_PASS', len(rows), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root')
    parser.add_argument('--cat', required=True)
    parser.add_argument('--zstd', required=True)
    parser.add_argument('--curl', required=True)
    parser.add_argument('--url', required=True)
    parser.add_argument('--repeats', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 8:
        parser.error('repeats must be between 1 and 8')
    run(args.root, args.cat, args.zstd, args.curl, args.url, args.repeats)

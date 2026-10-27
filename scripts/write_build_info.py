#!/usr/bin/env python3
"""Refresh one build-ID translation unit without recompiling unrelated sources."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

def write_build_info(root, output):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.DEVNULL)
    try:
        head = git('rev-parse', '--short', 'HEAD').decode().strip()
        branch = git('rev-parse', '--abbrev-ref', 'HEAD').decode().strip()
        date = git('log', '-1', '--format=%cs').decode().strip()
        diff = git('diff', '--binary', 'HEAD')
        untracked = git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0')
        digest = hashlib.sha256(diff)
        dirty = bool(diff)
        for name in sorted(name for name in untracked if name):
            file = root / name.decode()
            if file.is_file():
                dirty = True
                digest.update(name + b'\0')
                digest.update(file.read_bytes())
        if dirty:
            head += '+' + digest.hexdigest()[:8]
    except (OSError, subprocess.CalledProcessError):
        head, branch, date = 'unknown', 'unknown', 'unknown'
    content = '#include "build_info.h"\nnamespace brook {\n'
    for name, value in [('BuildGitHash', head), ('BuildGitBranch', branch), ('BuildDate', date)]:
        content += f'const char* {name}() {{ return {json.dumps(value)}; }}\n'
    content += '}\n'
    if not output.exists() or output.read_text() != content:
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_suffix('.tmp')
        temporary.write_text(content)
        temporary.replace(output)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    write_build_info(args.root.resolve(), args.output.resolve())

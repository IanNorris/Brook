#!/usr/bin/env python3
"""Prepare a private Vulkan Nix image offline when host FUSE is unavailable.

Run inside nix-shell from the repository root. The seed is only read; the output
must be new. Package roots and transitive dependencies come from the host store.
"""
import argparse
import json
import os
from pathlib import Path
import re
import stat
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seed', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--control', type=Path, default=Path('build/vulkan-triangle-control-result'))
parser.add_argument('--package', action='append', default=[], type=Path)
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
image = root / 'nix.img'
subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(args.seed.resolve()), str(image)], check=True)
control = json.loads((args.control / 'manifest.json').read_text())
roots = [control[k] for k in ('mesa', 'glvnd', 'vulkanLoader', 'vulkanTools', 'probe')]
roots += [str(p.resolve()) for p in args.package]
closure = sorted(set(subprocess.check_output(['nix-store', '-qR', *roots], text=True).splitlines()))
listing = subprocess.check_output(['debugfs', '-R', 'ls -p /store', str(image)], stderr=subprocess.DEVNULL, text=True)
present = {line.split('/')[5] for line in listing.splitlines() if line.startswith('/')}
commands, copied = [], []
files = 0
quote = json.dumps
for source in closure:
    src = Path(source)
    assert src.parent == Path('/nix/store') and src.is_dir(), source
    if src.name in present:
        continue
    copied.append(source)
    for parent, dirs, names in os.walk(src, followlinks=False):
        path = Path(parent)
        dest = '/store/' + str(path.relative_to(src.parent))
        commands += ['mkdir ' + quote(dest), 'set_inode_field ' + quote(dest) + ' mode 040555']
        for name in list(dirs):
            if (path / name).is_symlink():
                names.append(name)
                dirs.remove(name)
        for name in names:
            item, target = path / name, dest + '/' + name
            if item.is_symlink():
                commands.append('symlink ' + quote(target) + ' ' + quote(os.readlink(item)))
            else:
                assert item.is_file(), item
                mode = oct(stat.S_IFREG | stat.S_IMODE(item.stat().st_mode)).replace('0o', '0')
                commands += ['write ' + quote(str(item)) + ' ' + quote(target),
                             'set_inode_field ' + quote(target) + ' mode ' + mode]
                files += 1
batch = root / 'install.debugfs'
batch.write_text('\n'.join(commands) + '\n')
with (root / 'install.log').open('w') as log:
    subprocess.run(['debugfs', '-w', '-f', str(batch), str(image)], stdout=log, stderr=subprocess.STDOUT, check=True)
text = (root / 'install.log').read_text()
assert not re.search(r'Could not allocate|File not found|Usage:|not enough space|not a directory|already exists|No free|while opening|Invalid argument', text, re.I), 'See install.log'
with (root / 'fsck.log').open('w') as log:
    subprocess.run(['e2fsck', '-f', '-n', str(image)], stdout=log, stderr=subprocess.STDOUT, check=True)
(root / 'closure.json').write_text(json.dumps(dict(roots=roots, closure=closure, copied=copied, regular_files=files, method='offline debugfs; new private clone'), indent=2) + '\n')
print('VULKAN_NIX_SEED_STAGED', len(copied), 'of', len(closure), 'files', files)

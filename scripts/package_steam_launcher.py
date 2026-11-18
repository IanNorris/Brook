#!/usr/bin/env python3
"""Bundle the Dell Steam launcher for installation into user data.

    python3 scripts/package_steam_launcher.py [--output build/steam-launcher-bundle]

Writes <output>/ and <output>.tar with:
  brook-steam            scripts/workloads/brook_steam_launcher.py
  steam-client-check.py  scripts/workloads/steam_client_check.py
  install.py             verifies SHA256SUMS, backs up the installed copies,
                         installs both into /data/home/.local/share/brook/steam/
                         and runs `brook-steam --check`; `--rollback` restores
  SHA256SUMS, SOURCE.json

The destination is user data on the Dell, not /nix/store. Run install.py on
the Dell as the desktop user (uid 1000) with /nix/profile/bin/python3.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
FILES = {'brook-steam': ROOT / 'scripts/workloads/brook_steam_launcher.py',
         'steam-client-check.py': ROOT / 'scripts/workloads/steam_client_check.py'}

INSTALL = r'''#!/nix/profile/bin/python3
"""Install or roll back the Brook Steam launcher in the desktop user's data.

    /nix/profile/bin/python3 install.py            # verify, back up, install, --check
    /nix/profile/bin/python3 install.py --rollback # restore the newest backup set
"""
import hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
DEST = Path(os.environ.get('BROOK_STEAM_INSTALL_DIR', '/data/home/.local/share/brook/steam'))
NAMES = ('brook-steam', 'steam-client-check.py')

def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)

def copy_plain(source, target):
    # No shutil.copy2: copystat copies xattrs, and Brook's ext2 returns ENOSYS
    # for the xattr calls, which Python does not tolerate.
    shutil.copyfile(source, target)
    st = os.stat(source)
    os.chmod(target, st.st_mode & 0o7777)
    os.utime(target, ns=(st.st_atime_ns, st.st_mtime_ns))

def interpreter(script):
    line = script.read_bytes().split(b'\n', 1)[0].decode()
    path = line[2:].strip().split()[0] if line.startswith('#!') else ''
    if os.access(path, os.X_OK):
        return path
    print('warning: shebang interpreter', repr(path), 'not found; testing with', sys.executable)
    return sys.executable

IMPORT_TEST = (
    "import importlib.machinery, importlib.util, sys\n"
    "loader = importlib.machinery.SourceFileLoader('brook_steam', sys.argv[1])\n"
    "spec = importlib.util.spec_from_loader('brook_steam', loader)\n"
    "module = importlib.util.module_from_spec(spec); loader.exec_module(module)\n"
    "full = module.full_python() if module.ctypes is None else sys.executable\n"
    "print('launcher import ok under', sys.executable, '| ctypes:', module.ctypes is not None, '| full python:', full)\n")

def put(source, target):
    temporary = target.with_name(target.name + '.new')
    shutil.copyfile(source, temporary)
    os.chmod(temporary, 0o755)
    with open(temporary, 'rb') as stream: os.fsync(stream.fileno())
    os.replace(temporary, target)

if os.geteuid() == 0:
    sys.exit('run as the desktop user (uid 1000), not root')
if sys.argv[1:] == ['--rollback']:
    records = sorted(DEST.glob('rollback-*.json'))
    if not records: sys.exit('no rollback record in ' + str(DEST))
    record = json.loads(records[-1].read_text())
    for name, backup in record['backups'].items():
        if backup: put(Path(backup), DEST / name); print('restored', name, 'from', backup)
        else: (DEST / name).unlink(missing_ok=True); print('removed', name, '(was not installed before)')
    fsync_dir(DEST); records[-1].rename(records[-1].with_suffix('.restored')); sys.exit(0)
for line in (HERE / 'SHA256SUMS').read_text().splitlines():
    digest, name = line.split(None, 1)
    if hashlib.sha256((HERE / name.strip()).read_bytes()).hexdigest() != digest:
        sys.exit('checksum mismatch: ' + name)
DEST.mkdir(parents=True, exist_ok=True)
stamp = time.strftime('%Y%m%d-%H%M%S')
backups = {}
for name in NAMES:
    target = DEST / name
    if target.exists():
        backup = DEST / f'{name}.before-{stamp}'
        copy_plain(target, backup); backups[name] = str(backup)
    else:
        backups[name] = None
(DEST / f'rollback-{stamp}.json').write_text(json.dumps(dict(backups=backups,
    source=json.loads((HERE / 'SOURCE.json').read_text())), indent=2) + '\n')
for name in NAMES:
    put(HERE / name, DEST / name)
fsync_dir(DEST)
print('installed', ', '.join(NAMES), 'into', DEST, '; backups:', backups)
python = interpreter(DEST / 'brook-steam')
if subprocess.call([python, '-c', IMPORT_TEST, str(DEST / 'brook-steam')]):
    sys.exit('launcher failed to import under ' + python + '; roll back with: install.py --rollback')
sys.exit(subprocess.call([python, str(DEST / 'brook-steam'), '--check']))
'''

README = '''Brook Steam launcher bundle ({commit})

Default: rootless Steam on the desktop Xwayland (:0) with GPU OpenGL and a
launcher-owned PulseAudio null sink. Fallback: brook-steam --xephyr.

On the Dell, as uid 1000, after copying this directory there:
  /nix/profile/bin/python3 install.py
  /data/home/.local/share/brook/steam/brook-steam --probe
Roll back:
  /nix/profile/bin/python3 install.py --rollback
See docs/desktop-game-launchers.md, "Rootless Steam on the Dell".
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/steam-launcher-bundle')
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    commit = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'],
                            capture_output=True, text=True).stdout.strip() or 'unknown'
    dirty = bool(subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--'] + [str(p) for p in FILES.values()],
                                capture_output=True, text=True).stdout.strip())
    for name, source in FILES.items():
        shutil.copyfile(source, out / name)
        (out / name).chmod(0o755)
    (out / 'install.py').write_text(INSTALL)
    (out / 'install.py').chmod(0o755)
    (out / 'README.txt').write_text(README.format(commit=commit + (' + uncommitted' if dirty else '')))
    (out / 'SOURCE.json').write_text(json.dumps(dict(commit=commit, dirty=dirty,
                                                     files={n: str(p.relative_to(ROOT)) for n, p in FILES.items()}),
                                                indent=2) + '\n')
    names = ['brook-steam', 'steam-client-check.py', 'install.py', 'README.txt', 'SOURCE.json']
    (out / 'SHA256SUMS').write_text(''.join(f'{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n'
                                            for n in names))
    archive = out.with_suffix('.tar')
    with tarfile.open(archive, 'w') as tar:
        tar.add(out, arcname=out.name)
    print(json.dumps(dict(bundle=str(out), archive=str(archive), commit=commit, dirty=dirty,
                          sha256=hashlib.sha256(archive.read_bytes()).hexdigest())))


if __name__ == '__main__':
    os.umask(0o022)
    main()

#!/usr/bin/env python3
"""Host side: build a create-only staging bundle for new store paths plus
/nix/profile symlink swaps (the pattern used for the GPU browser launchers and
brook-vaapi). usage:
  make_bundle.py OUTDIR --path STORE_PATH... --link PROFILE_LINK=TARGET...
Writes OUTDIR/{store.tar,manifest.json,closure.txt,links.json,install.py} and
OUTDIR/bundle.tar holding all five. closure.txt is the full runtime closure of
the staged paths; install.py refuses to swap links unless all of it is present."""
import argparse, hashlib, json, os, shutil, subprocess, sys, tarfile
ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--path', action='append', required=True)
ap.add_argument('--link', action='append', default=[])
a = ap.parse_args()
os.makedirs(a.out, exist_ok=False)
def sha(f):
    h = hashlib.sha256()
    with open(f, 'rb') as s:
        for b in iter(lambda: s.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
man = {}
for p in a.path:
    assert p.startswith('/nix/store/') and '/' not in p[len('/nix/store/'):], p
    if os.path.islink(p): man[p] = {'link': os.readlink(p)}; continue
    if os.path.isfile(p): man[p] = {'file': sha(p), 'exec': bool(os.stat(p).st_mode & 0o111)}; continue
    e = {'files': {}, 'links': {}, 'dirs': [], 'exec': []}
    for d, dirs, files in os.walk(p):
        for n in sorted(dirs) + sorted(files):
            f = os.path.join(d, n); r = os.path.relpath(f, p)
            if os.path.islink(f): e['links'][r] = os.readlink(f)
            elif os.path.isdir(f): e['dirs'].append(r)
            else:
                e['files'][r] = sha(f)
                if os.stat(f).st_mode & 0o111: e['exec'].append(r)
    man[p] = e
links = {}
for spec in a.link:
    k, v = spec.split('=', 1)
    assert k.startswith('/nix/profile/') and v.startswith('/nix/store/'), spec
    assert any(v == p or v.startswith(p + '/') for p in a.path), ('link target not staged', v)
    links[k] = v
closure = subprocess.run(['nix-store', '-qR'] + a.path, check=True, capture_output=True, text=True).stdout.split()
with open(os.path.join(a.out, 'manifest.json'), 'w') as f: json.dump(man, f, indent=1, sort_keys=True)
with open(os.path.join(a.out, 'links.json'), 'w') as f: json.dump(links, f, indent=1, sort_keys=True)
with open(os.path.join(a.out, 'closure.txt'), 'w') as f: f.write(''.join(c + '\n' for c in sorted(closure)))
with tarfile.open(os.path.join(a.out, 'store.tar'), 'w', format=tarfile.PAX_FORMAT) as t:
    def root_owned(m):
        m.uid = m.gid = 0; m.uname = m.gname = 'root'; return m
    for p in a.path: t.add(p, p.lstrip('/'), filter=root_owned)
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'install.py'), os.path.join(a.out, 'install.py'))
with tarfile.open(os.path.join(a.out, 'bundle.tar'), 'w') as t:
    for n in ('store.tar', 'manifest.json', 'closure.txt', 'links.json', 'install.py'): t.add(os.path.join(a.out, n), n)
print(os.path.join(a.out, 'bundle.tar'), sha(os.path.join(a.out, 'bundle.tar')))

#!/usr/bin/env python3
"""Host side: build the create-only staging bundle for store paths.
usage: make_bundle.py MISSING.txt CLOSURE.txt OUTDIR
Writes OUTDIR/{store.tar,manifest.json,closure.txt,install.py,bundle.tar}."""
import hashlib, json, os, shutil, stat, sys, tarfile
missing, closure, out = sys.argv[1:4]
os.makedirs(out, exist_ok=False)
paths = [l.strip() for l in open(missing) if l.strip()]
def sha(f):
    h = hashlib.sha256()
    with open(f, 'rb') as s:
        for b in iter(lambda: s.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
man = {}
for p in paths:
    if os.path.islink(p): man[p] = {'link': os.readlink(p)}; continue
    if os.path.isfile(p): man[p] = {'file': sha(p), 'exec': bool(os.stat(p).st_mode & 0o111)}; continue
    e = {'files': {}, 'links': {}, 'dirs': [], 'exec': []}
    for d, dirs, files in os.walk(p):
        for n in dirs + files:
            f = os.path.join(d, n); r = os.path.relpath(f, p)
            if os.path.islink(f): e['links'][r] = os.readlink(f)
            elif os.path.isdir(f): e['dirs'].append(r)
            else:
                e['files'][r] = sha(f)
                if os.stat(f).st_mode & 0o111: e['exec'].append(r)
    man[p] = e
json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=1, sort_keys=True)
with tarfile.open(os.path.join(out, 'store.tar'), 'w') as t:
    for p in paths: t.add(p, p.lstrip('/'))
shutil.copy(closure, os.path.join(out, 'closure.txt'))
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'install.py'), os.path.join(out, 'install.py'))
with tarfile.open(os.path.join(out, 'bundle.tar'), 'w') as t:
    for n in ('store.tar', 'manifest.json', 'closure.txt', 'install.py'): t.add(os.path.join(out, n), n)
print(os.path.join(out, 'bundle.tar'), sha(os.path.join(out, 'bundle.tar')))

# Guest-side (root) create-only installer for store paths (VA-API phase 2a).
# usage: install.py BUNDLE.tar MANIFEST.json CLOSURE.txt TXID
# Refuses if any manifest path already exists; extracts into a private stage
# directory under /nix/store, verifies every file hash, symlink target and the
# exact entry set, renames each path into place, re-verifies, then checks the
# whole closure is present. Never overwrites, removes or touches /nix/profile.
import hashlib, json, os, sys, tarfile
tarpath, manpath, closurepath, tx = sys.argv[1:5]
man = json.load(open(manpath))
closure = [l.strip() for l in open(closurepath) if l.strip()]
def sha(f):
    h = hashlib.sha256()
    with open(f, 'rb') as s:
        for b in iter(lambda: s.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
def verify(root, p, e):
    if 'file' in e:
        assert os.path.isfile(root) and not os.path.islink(root) and sha(root) == e['file'], ('file', p)
        assert bool(os.stat(root).st_mode & 0o111) == e['exec'], ('mode', p); return
    if 'link' in e:
        assert os.path.islink(root) and os.readlink(root) == e['link'], ('link', p); return
    assert os.path.isdir(root) and not os.path.islink(root), ('dir', p)
    seen = set()
    for d, dirs, files in os.walk(root):
        for n in dirs + files:
            f = os.path.join(d, n); r = os.path.relpath(f, root); seen.add(r)
            if os.path.islink(f): assert e['links'].get(r) == os.readlink(f), ('link', p, r)
            elif os.path.isfile(f):
                assert e['files'].get(r) == sha(f), ('hash', p, r)
                assert bool(os.stat(f).st_mode & 0o111) == (r in e['exec']), ('mode', p, r)
    want = set(e['files']) | set(e['links']) | set(e['dirs'])
    assert seen == want, ('set', p, sorted(seen ^ want)[:5])
for p in man:
    assert p.startswith('/nix/store/') and '/' not in p[len('/nix/store/'):], p
    assert not os.path.lexists(p), 'refusing: already present ' + p
stage = '/nix/store/.brook-vaapi-stage-' + tx
os.mkdir(stage, 0o700)
with tarfile.open(tarpath) as t:
    for m in t.getmembers():
        assert m.name.startswith('nix/store/') and '..' not in m.name.split('/'), m.name
        assert m.isdir() or m.isfile() or m.issym(), m.name
    t.extractall(stage, filter='tar')
for p, e in man.items():
    verify(stage + p, p, e)
for p in man:
    os.rename(stage + p, p); print('INSTALLED', p)
for p, e in man.items(): verify(p, p, e)
os.sync()
for d in ('nix/store', 'nix', ''):
    os.rmdir(os.path.join(stage, d)) if d else os.rmdir(stage)
missing = [p for p in closure if not os.path.lexists(p)]
print('CLOSURE', len(closure), 'MISSING', len(missing))
for p in missing: print('MISSING', p)
print('VAAPI_STAGE_OK' if not missing else 'VAAPI_STAGE_INCOMPLETE')

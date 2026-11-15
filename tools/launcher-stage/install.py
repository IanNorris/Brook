# Guest side (root): create-only, hash-verified store install, then atomic
# /nix/profile symlink swaps with a rollback record.
# usage: install.py STORE.tar MANIFEST.json CLOSURE.txt LINKS.json TX [--check]
# --check only reports which manifest paths already exist and which closure
# paths are missing; it writes nothing. A normal run refuses if any manifest
# path already exists or any closure path (other than the staged ones) is
# missing, extracts into a private /nix/store/.brook-launcher-stage-TX,
# verifies every file hash, exec bit, symlink and the exact entry set, renames
# each path into place and re-verifies, then records the previous link targets
# in /nix/.brook-profile-rollback-TX.json before swapping each link.
# Rollback: for each entry of "previous", point the link back (or remove it if
# it was null). BROOK_STAGE_ROOT prefixes every absolute path (host tests).
import hashlib, json, os, sys, tarfile, time
root = os.environ.get('BROOK_STAGE_ROOT', '')
tarpath, manpath, closurepath, linkpath, tx = sys.argv[1:6]
check = '--check' in sys.argv[6:]
assert tx.isalnum(), tx
man = json.load(open(manpath)); links = json.load(open(linkpath))
closure = [l.strip() for l in open(closurepath) if l.strip()]
R = lambda p: root + p
def sha(f):
    h = hashlib.sha256()
    with open(f, 'rb') as s:
        for b in iter(lambda: s.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
def verify(path, p, e):
    if 'file' in e:
        assert os.path.isfile(path) and not os.path.islink(path) and sha(path) == e['file'], ('file', p)
        assert bool(os.stat(path).st_mode & 0o111) == e['exec'], ('mode', p); return
    if 'link' in e:
        assert os.path.islink(path) and os.readlink(path) == e['link'], ('link', p); return
    assert os.path.isdir(path) and not os.path.islink(path), ('dir', p)
    seen = set()
    for d, dirs, files in os.walk(path):
        for n in dirs + files:
            f = os.path.join(d, n); r = os.path.relpath(f, path); seen.add(r)
            if os.path.islink(f): assert e['links'].get(r) == os.readlink(f), ('link', p, r)
            elif os.path.isfile(f):
                assert e['files'].get(r) == sha(f), ('hash', p, r)
                assert bool(os.stat(f).st_mode & 0o111) == (r in e['exec']), ('mode', p, r)
    want = set(e['files']) | set(e['links']) | set(e['dirs'])
    assert seen == want, ('set', p, sorted(seen ^ want)[:5])
present = [p for p in man if os.path.lexists(R(p))]
missing = [p for p in closure if p not in man and not os.path.lexists(R(p))]
for p in present: print('PRESENT', p)
for p in missing: print('MISSING', p)
for k in links: print('LINK', k, '->', os.readlink(R(k)) if os.path.islink(R(k)) else None)
if check:
    print('CHECK_OK' if not present and not missing else 'CHECK_BLOCKED'); sys.exit(0)
assert not present, 'refusing: already present'
assert not missing, 'refusing: closure incomplete'
for k, v in links.items():
    assert k.startswith('/nix/profile/') and any(v == p or v.startswith(p + '/') for p in man), (k, v)
    assert not os.path.lexists(R(k)) or os.path.islink(R(k)), ('not a link', k)
for p in man: assert p.startswith('/nix/store/') and '/' not in p[len('/nix/store/'):], p
stage = R('/nix/store/.brook-launcher-stage-' + tx)
os.mkdir(stage, 0o700)
with tarfile.open(tarpath) as t:
    for m in t.getmembers():
        assert m.name.startswith('nix/store/') and '..' not in m.name.split('/'), m.name
        assert m.isdir() or m.isfile() or m.issym(), ('type', m.name)
    t.extractall(stage, filter='tar')
for p, e in man.items(): verify(stage + p, p, e)
for p in man: os.rename(stage + p, R(p)); print('INSTALLED', p)
for p, e in man.items(): verify(R(p), p, e)
os.sync()
for d in ('nix/store', 'nix', ''): os.rmdir(os.path.join(stage, d)) if d else os.rmdir(stage)
for v in links.values(): assert os.path.isfile(R(v)) and os.access(R(v), os.X_OK), ('target', v)
previous = {k: (os.readlink(R(k)) if os.path.islink(R(k)) else None) for k in links}
rb = '/nix/.brook-profile-rollback-' + tx + '.json'
with open(R(rb), 'w') as f:
    json.dump({'time': time.time(), 'previous': previous, 'new': links}, f, indent=1); f.flush(); os.fsync(f.fileno())
print('ROLLBACK_RECORD', rb, json.dumps(previous))
for k, v in links.items():
    tmp = R(k) + '.brook-new'
    if os.path.lexists(tmp): os.unlink(tmp)
    os.symlink(v, tmp); os.replace(tmp, R(k)); assert os.readlink(R(k)) == v
    print('PROFILE', k, '->', v)
os.sync(); print('LAUNCHER_STAGE_OK')

# Guest side (root): create-only, hash-verified store install, then atomic
# /nix/profile symlink swaps with a rollback record, and optionally Apps-menu
# updates.
# usage: install.py STORE.tar MANIFEST.json CLOSURE.txt LINKS.json TX [--check] [--apps APPS.json]
#        install.py --rollback TX
# --check only reports which manifest paths already exist, which closure paths
# are missing and the planned Apps changes; it writes nothing. A normal run
# refuses if any manifest path already exists or any closure path (other than
# the staged ones) is missing, extracts into a private
# /nix/store/.brook-launcher-stage-TX, verifies every file hash, exec bit,
# symlink and the exact entry set, renames each path into place and
# re-verifies, then records the previous link targets (and a copy of the
# launcher manifest) in /nix/.brook-profile-rollback-TX.json before swapping
# each link. Links under /nix/profile/bin must target executables; other links
# (desktop entries) must target regular files.
# APPS.json (optional) edits /nix/share/applications/applications.idx, the
# kernel launcher's manifest (Name, Exec, Icon, Categories, app id):
#   "icons":   {"NAME.rgba": "/nix/store/...", ...}  copied into icons/ if absent
#   "upsert":  [{"app_id","name","exec","icon","categories"}]  replaces the
#              line with the same app id or name, else appends
#   "rewrite_exec": [{"program": "codium", "exec": "--uid 1000 /nix/profile/bin/brook-codium"}]
#              replaces the command of every line whose executable basename is
#              `program`, keeping its trailing arguments
# The manifest is replaced atomically and the desktop is notified (syscall
# 0xB03) so the Apps menu reloads without a reboot.
# --rollback TX restores each link's previous target (removing it if it was
# absent), restores the saved manifest, removes icons this run created and
# notifies the desktop. Store paths are left in place.
# BROOK_STAGE_ROOT prefixes every absolute path (host tests) and suppresses
# the desktop notification.
import hashlib, json, os, shutil, sys, tarfile, time
root = os.environ.get('BROOK_STAGE_ROOT', '')
R = lambda p: root + p
IDX = '/nix/share/applications/applications.idx'
ICONS = '/nix/share/applications/icons/'
def notify():
    if root: return
    try:
        import ctypes
        ctypes.CDLL(None, use_errno=True).syscall(0xB03); print('APPS_NOTIFIED')
    except Exception as e: print('APPS_NOTIFY_FAILED', e, '(the menu reloads at next boot)')
def write_atomic(path, data):
    tmp = path + '.brook-new'
    with open(tmp, 'w') as f: f.write(data); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)
def rollback_record(tx): return '/nix/.brook-profile-rollback-' + tx + '.json'

if sys.argv[1:2] == ['--rollback']:
    tx = sys.argv[2]; assert tx.isalnum(), tx
    rb = json.load(open(R(rollback_record(tx))))
    for k, v in rb['previous'].items():
        if v is None:
            if os.path.islink(R(k)): os.unlink(R(k))
            print('UNLINKED', k)
        else:
            tmp = R(k) + '.brook-new'
            if os.path.lexists(tmp): os.unlink(tmp)
            os.symlink(v, tmp); os.replace(tmp, R(k)); print('PROFILE', k, '->', v)
    apps = rb.get('apps')
    if apps:
        if apps['idx_backup'] is None:
            if os.path.exists(R(IDX)): os.unlink(R(IDX))
        else: write_atomic(R(IDX), open(R(apps['idx_backup'])).read())
        print('APPS_RESTORED', IDX)
        for icon in apps['icons_created']:
            if os.path.isfile(R(ICONS + icon)): os.unlink(R(ICONS + icon)); print('ICON_REMOVED', icon)
        os.sync(); notify()
    os.sync(); print('ROLLBACK_OK'); sys.exit(0)

tarpath, manpath, closurepath, linkpath, tx = sys.argv[1:6]
rest = sys.argv[6:]
check = '--check' in rest
appspath = rest[rest.index('--apps') + 1] if '--apps' in rest else None
assert tx.isalnum(), tx
man = json.load(open(manpath)); links = json.load(open(linkpath))
apps = json.load(open(appspath)) if appspath else None
closure = [l.strip() for l in open(closurepath) if l.strip()]
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
def staged(v): return any(v == p or v.startswith(p + '/') for p in man)

def exec_program(command):
    """Basename of the executable after optional --uid/--gid N pairs, and the
    argument suffix after it; None if unparseable."""
    words = command.split(' ')
    i = 0
    while i + 1 < len(words) and words[i] in ('--uid', '--gid'): i += 2
    if i >= len(words) or not words[i]: return None
    return os.path.basename(words[i]), ''.join(' ' + w for w in words[i + 1:])
def plan_apps(old):
    """Returns (new manifest text, report lines)."""
    lines = [l for l in old.split('\n') if l.strip()]
    report = []
    for rule in apps.get('rewrite_exec', []):
        hits = 0
        for i, line in enumerate(lines):
            f = line.split('\t')
            if len(f) < 2: continue
            parsed = exec_program(f[1])
            if not parsed or parsed[0] != rule['program']: continue
            f[1] = rule['exec'] + parsed[1]; lines[i] = '\t'.join(f); hits += 1
            report.append('APPS_REWRITE %s: %s' % (f[0], f[1]))
        if not hits: report.append('APPS_REWRITE_NONE %s (no entry runs it)' % rule['program'])
    for e in apps.get('upsert', []):
        new = '\t'.join([e['name'], e['exec'], e.get('icon', ''), e.get('categories', ''), e['app_id']])
        assert '\n' not in new and len(new) < 1024, e
        for i, line in enumerate(lines):
            f = line.split('\t')
            if f[0] == e['name'] or (len(f) >= 5 and f[4] == e['app_id']):
                lines[i] = new; report.append('APPS_REPLACE ' + new.replace('\t', ' | ')); break
        else:
            lines.append(new); report.append('APPS_ADD ' + new.replace('\t', ' | '))
    text = ''.join(l + '\n' for l in lines)
    assert len(text.encode()) <= 64 * 1024 and '\0' not in text
    return text, report

present = [p for p in man if os.path.lexists(R(p))]
missing = [p for p in closure if p not in man and not os.path.lexists(R(p))]
for p in present: print('PRESENT', p)
for p in missing: print('MISSING', p)
for k in links: print('LINK', k, '->', os.readlink(R(k)) if os.path.islink(R(k)) else None)
old_idx = None
if apps is not None:
    old_idx = open(R(IDX)).read() if os.path.isfile(R(IDX)) else None
    new_idx, report = plan_apps(old_idx or '')
    for line in report: print(line)
    for icon, src in apps.get('icons', {}).items():
        assert '/' not in icon and icon.endswith('.rgba') and staged(src), (icon, src)
        print('ICON', icon, 'exists' if os.path.lexists(R(ICONS + icon)) else 'new')
if check:
    print('CHECK_OK' if not present and not missing else 'CHECK_BLOCKED'); sys.exit(0)
assert not present, 'refusing: already present'
assert not missing, 'refusing: closure incomplete'
for k, v in links.items():
    assert k.startswith('/nix/profile/') and staged(v), (k, v)
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
for k, v in links.items():
    assert os.path.isfile(R(v)), ('target', v)
    if k.startswith('/nix/profile/bin/'): assert os.access(R(v), os.X_OK), ('target not executable', v)
previous = {k: (os.readlink(R(k)) if os.path.islink(R(k)) else None) for k in links}
rb = rollback_record(tx)
record = {'time': time.time(), 'previous': previous, 'new': links}
if apps is not None:
    backup = None
    if old_idx is not None:
        backup = '/nix/.brook-apps-rollback-' + tx + '.idx'
        write_atomic(R(backup), old_idx)
    record['apps'] = {'idx_backup': backup,
                      'icons_created': [i for i in apps.get('icons', {}) if not os.path.lexists(R(ICONS + i))]}
with open(R(rb), 'w') as f:
    json.dump(record, f, indent=1); f.flush(); os.fsync(f.fileno())
print('ROLLBACK_RECORD', rb, json.dumps(previous))
for k, v in links.items():
    os.makedirs(os.path.dirname(R(k)), exist_ok=True)
    tmp = R(k) + '.brook-new'
    if os.path.lexists(tmp): os.unlink(tmp)
    os.symlink(v, tmp); os.replace(tmp, R(k)); assert os.readlink(R(k)) == v
    print('PROFILE', k, '->', v)
if apps is not None:
    os.makedirs(R(ICONS), exist_ok=True)
    for icon in record['apps']['icons_created']:
        shutil.copyfile(R(apps['icons'][icon]), R(ICONS + icon) + '.brook-new')
        os.replace(R(ICONS + icon) + '.brook-new', R(ICONS + icon)); print('ICON_INSTALLED', icon)
    current = open(R(IDX)).read() if os.path.isfile(R(IDX)) else None
    assert current == old_idx, 'launcher manifest changed during install; rerun'
    write_atomic(R(IDX), new_idx); print('APPS_UPDATED', IDX)
    os.sync(); notify()
os.sync(); print('LAUNCHER_STAGE_OK')

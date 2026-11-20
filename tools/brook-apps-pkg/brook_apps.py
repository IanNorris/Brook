"""Brook user app index: freedesktop entries a user installed (web apps,
Chromium "Install app", Flatpak exports) become launcher entries.

The kernel launcher reads two tab-separated manifests (Name, Command, Icon,
Categories, app_id): the root-owned system one under /nix/share/applications
and this per-user one under ~/.local/share/brook. The kernel re-validates every
user line and always runs it as the desktop user, so this module only writes
lines that already satisfy those rules:

- the command's first token is an executable installed in /nix/profile/bin
  (written as that absolute path) and never starts with '-';
- arguments are printable ASCII without quotes, backslashes or controls;
- the icon is a bare file name in the user icon directory.
"""

import configparser
import io
import os
import shlex
import struct
import tempfile
from pathlib import Path

ICON_SIZE = 24
PROFILE_BIN = Path('/nix/profile/bin')
COMMAND_LIMIT = 127 - len("--uid 1000 ")  # kernel: 127 bytes including its run prefix
APP_ID_LIMIT = 63
# The kernel splits the command on spaces and starts the program directly (no
# shell), so only its own quoting characters and controls are unsafe.
SAFE_ARG = set(chr(c) for c in range(0x21, 0x7f)) - set('"\'\\`')
FIELD_CODES = {'%f', '%F', '%u', '%U', '%i', '%c', '%k', '%d', '%D', '%n', '%N', '%v', '%m'}
# Upstream launchers that Brook runs through its own wrapper (GPU flags,
# profile, sandbox policy); remaining arguments are kept.
WRAPPED = {'chromium': 'brook-chromium', 'chromium-browser': 'brook-chromium',
           'google-chrome': 'brook-chromium', 'google-chrome-stable': 'brook-chromium',
           'firefox': 'brook-firefox'}


def home():
    return Path(os.environ.get('HOME') or '/data/home')


def data_home():
    return Path(os.environ.get('XDG_DATA_HOME') or home() / '.local/share')


def entry_dirs():
    """Directories whose .desktop files belong in the user index, in
    precedence order (an earlier directory wins on the same file name)."""
    return [data_home() / 'applications',
            data_home() / 'flatpak/exports/share/applications',
            Path('/var/lib/flatpak/exports/share/applications')]


def icon_roots():
    return [data_home() / 'icons',
            data_home() / 'flatpak/exports/share/icons',
            Path('/var/lib/flatpak/exports/share/icons'),
            Path('/nix/profile/share/icons'),
            Path('/nix/profile/share/pixmaps')]


def index_dir():
    return data_home() / 'brook'


def parse_desktop(path):
    """The fields Brook needs from a desktop file, or None when it is not a
    visible application."""
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.optionxform = str
    try:
        cp.read(path, encoding='utf-8')
    except (configparser.Error, UnicodeDecodeError, OSError):
        return None
    if not cp.has_section('Desktop Entry'):
        return None
    e = cp['Desktop Entry']
    if e.get('Type', 'Application') != 'Application':
        return None
    for key in ('Hidden', 'NoDisplay'):
        if e.get(key, 'false').strip().lower() == 'true':
            return None
    name, exec_line = e.get('Name', '').strip(), e.get('Exec', '').strip()
    if not name or not exec_line:
        return None
    return {'name': name, 'exec': exec_line, 'icon': e.get('Icon', '').strip(),
            'categories': e.get('Categories', '').strip(),
            'wmclass': e.get('StartupWMClass', '').strip(),
            'webapp': e.get('X-Brook-WebApp', '').strip(),
            'id': Path(path).stem}


def translate_exec(exec_line, profile_bin=PROFILE_BIN):
    """The launcher command for a desktop Exec line, or None when it cannot be
    expressed safely (env assignments, shell syntax, programs outside the
    profile)."""
    try:
        argv = [a for a in shlex.split(exec_line) if a not in FIELD_CODES]
    except ValueError:
        return None
    if not argv or '=' in argv[0]:
        return None
    program = os.path.basename(argv[0])
    program = WRAPPED.get(program, program)
    if not program or program.startswith('-') or not (profile_bin / program).exists():
        return None
    args = argv[1:]
    if any(a == '--uid' or a.startswith('--uid=') for a in args):
        return None
    if any(not a or any(ch not in SAFE_ARG for ch in a) for a in args):
        return None
    command = ' '.join([str(profile_bin / program)] + args)
    return command if len(command) <= COMMAND_LIMIT else None


def find_icon(icon):
    """A file for the Icon= value: an absolute path, or the largest hicolor
    (then any theme) match."""
    if not icon:
        return None
    if icon.startswith('/'):
        return Path(icon) if Path(icon).is_file() else None
    stem = icon[:-4] if icon.endswith(('.png', '.svg', '.xpm')) else icon
    best = None
    for root in icon_roots():
        if not root.is_dir():
            continue
        if root.name == 'pixmaps':
            for ext in ('.png', '.svg'):
                if (root / (stem + ext)).is_file():
                    return root / (stem + ext)
            continue
        for theme in sorted(root.iterdir(), key=lambda p: p.name != 'hicolor'):
            for path in theme.glob(f'*/apps/{stem}.*'):
                if path.suffix not in ('.png', '.svg'):
                    continue
                size = path.parent.parent.name
                score = 10000 if size == 'scalable' else int(size.split('x')[0]) if size.split('x')[0].isdigit() else 0
                if best is None or score > best[0]:
                    best = (score, path)
        if best:
            return best[1]
    return None


def icon_bytes(path, size=ICON_SIZE):
    """Raw size x size little-endian 0xAARRGGBB (straight alpha), as the
    kernel launcher loads icons; None if the image cannot be decoded."""
    from PIL import Image, ImageOps
    try:
        if str(path).lower().endswith('.svg'):
            import cairosvg
            source = io.BytesIO(cairosvg.svg2png(url=str(path), output_width=size * 4, output_height=size * 4))
        else:
            source = path
        img = Image.open(source).convert('RGBA')
    except Exception:
        return None
    img = ImageOps.contain(img, (size, size), Image.Resampling.LANCZOS)
    canvas = Image.new('RGBA', (size, size))
    canvas.alpha_composite(img, ((size - img.width) // 2, (size - img.height) // 2))
    out = bytearray()
    for r, g, b, a in canvas.getdata():
        out += struct.pack('<I', (a << 24) | (r << 16) | (g << 8) | b)
    return bytes(out)


def safe_name(text, limit=48):
    out = ''.join(c if c.isalnum() or c in '-_.' else '_' for c in text.lower())[:limit].strip('._')
    return out or 'app'


def clean_field(text):
    return ' '.join(text.replace('\t', ' ').split())


def build_index(dirs=None, profile_bin=PROFILE_BIN):
    """(lines, icons, skipped): manifest lines, {icon file: bytes} and the
    entries left out with the reason."""
    seen, lines, icons, skipped = set(), [], {}, []
    for directory in dirs if dirs is not None else entry_dirs():
        if not Path(directory).is_dir():
            continue
        for path in sorted(Path(directory).glob('*.desktop')):
            if path.name in seen:
                continue
            seen.add(path.name)
            entry = parse_desktop(path)
            if not entry:
                continue
            command = translate_exec(entry['exec'], profile_bin)
            if not command:
                skipped.append((str(path), 'exec'))
                continue
            app_id = clean_field(entry['wmclass'] or entry['id'])[:APP_ID_LIMIT]
            icon_file = ''
            source = find_icon(entry['icon'])
            if source:
                data = icon_bytes(source)
                if data:
                    icon_file = safe_name(app_id) + '.rgba'
                    icons[icon_file] = data
            categories = clean_field(entry['categories'])
            if entry['webapp']:
                # The desktop titles web-app windows with the app's name:
                # Chromium titles --app windows with the bare host.
                categories = (categories + ';' if categories and not categories.endswith(';') else categories) + 'X-Brook-WebApp;'
            lines.append('\t'.join([clean_field(entry['name']), command, icon_file, categories, app_id]))
    return lines, icons, skipped


def _atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix='.' + path.name + '.')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


def write_index(lines, icons, out=None):
    """Write icons, then the manifest (so a reader never sees a line whose
    icon is missing), and remove icons no line uses. True if the manifest
    changed."""
    out = Path(out) if out else index_dir()
    icon_dir = out / 'icons'
    icon_dir.mkdir(parents=True, exist_ok=True)
    for name, data in icons.items():
        target = icon_dir / name
        if not target.is_file() or target.read_bytes() != data:
            _atomic_write(target, data)
    manifest = out / 'user-apps.idx'
    text = ('\n'.join(lines) + '\n' if lines else '').encode()
    changed = not manifest.is_file() or manifest.read_bytes() != text
    if changed:
        _atomic_write(manifest, text)
    for stale in icon_dir.glob('*.rgba'):
        if stale.name not in icons:
            stale.unlink()
    return changed


def notify():
    """Ask the desktop to reload the Apps catalog (best effort)."""
    try:
        import ctypes
        ctypes.CDLL(None, use_errno=True).syscall(0xB03)
    except Exception:
        pass

#!/nix/profile/bin/python3
"""Launch the Dell's installed Steam client.

Default (rootless): Steam's windows join the desktop's own rootless Xwayland
(DISPLAY :0, glamor on Iris), and the client and its games use hardware
OpenGL through that server's GLX. `--xephyr` keeps the previous nested
software display (Xephyr :14 with llvmpipe) as a fallback.

In both modes the launcher owns a private null-output PulseAudio server for
Steam (without one, libpulse autospawn loops and exhausts descriptors). Brook
has no system tray, so a Steam client without a window is unreachable and
blocks a graceful restart: once Steam has shown a window, the launcher ends
it after a grace period with no Steam window and no running game. On SIGTERM,
SIGINT or SIGHUP, or when the client exits, it stops the whole client tree
and the audio server within a bounded time.

`--check` verifies prerequisites and the target display; `--probe` also
creates a GLX context on it and reports the renderer. Design notes:
docs/desktop-game-launchers.md ("Rootless Steam on the Dell").
"""
import argparse
import glob
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
try:
    import ctypes
except ImportError:  # python3-minimal, the Dell's /nix/profile/bin/python3, lacks _ctypes
    ctypes = None

LAUNCHER_VERSION = 'rootless-2'
HOME = Path(os.environ.get('BROOK_STEAM_USER_HOME', '/data/home'))
BROOK = HOME / '.local/share/brook'
LOG = Path(os.environ.get('BROOK_STEAM_LOG', BROOK / 'steam-launcher.log'))
STATE = Path(os.environ.get('BROOK_STEAM_STATE', BROOK / 'steam-launcher-state.json'))
PIDFILE = Path(os.environ.get('BROOK_STEAM_PIDFILE', BROOK / 'steam-launcher.pid'))
RESOLV = Path(os.environ.get('BROOK_STEAM_RESOLV', '/etc/RESOLV.CONF'))
PULSE_PREFIX = 'brook-steam-pulse-'
X_EXTENSIONS = ('GLX', 'DRI3', 'Present', 'MIT-SHM', 'XFIXES', 'Composite', 'RANDR',
                'XInputExtension', 'DRI2')
_log_stream = None


def log(message, **fields):
    line = time.strftime('%Y-%m-%d %H:%M:%S') + ' brook-steam: ' + message
    if fields:
        line += ' ' + json.dumps(fields, sort_keys=True, default=str)
    try:
        stream = _log_stream or LOG.open('a')
        stream.write(line + '\n')
        stream.flush()
        if stream is not _log_stream:
            stream.close()
    except OSError:
        pass
    return line


# ---------------------------------------------------------------------------
# Minimal X11 protocol client. Pure Python on purpose: libX11 exits the whole
# process on a lost connection, and the desktop compositor can restart its
# Xwayland generation while Steam runs.
# ---------------------------------------------------------------------------
class XError(Exception):
    pass


def display_number(display):
    match = re.fullmatch(r'(?:unix)?:(\d+)(?:\.\d+)?', display or '')
    if not match:
        raise XError(f'unsupported DISPLAY {display!r}; expected a local :N display')
    return int(match.group(1))


class XConnection:
    WM_NAME, WM_CLASS, STRING = 39, 67, 31

    def __init__(self, display, timeout=3.0):
        number = display_number(display)
        self.display = display
        self.path = f'/tmp/.X11-unix/X{number}'
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        try:
            self.sock.connect(self.path)
        except OSError:
            self.sock.close()
            raise
        try:
            self._setup()
        except BaseException:
            self.sock.close()
            raise

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _read(self, count):
        data = bytearray()
        while len(data) < count:
            chunk = self.sock.recv(count - len(data))
            if not chunk:
                raise XError('X server closed the connection')
            data += chunk
        return bytes(data)

    def _setup(self):
        # Little-endian, protocol 11.0, no authorization: Brook's owned
        # Xwayland runs with -ac and Xephyr is local to this launcher.
        self.sock.sendall(struct.pack('<BxHHHHxx', ord('l'), 11, 0, 0, 0))
        status, reason_length, _, _, length = struct.unpack('<BBHHH', self._read(8))
        data = self._read(length * 4)
        if status != 1:
            reason = data[:reason_length].decode(errors='replace') if status == 0 else 'authentication required'
            raise XError(f'X connection refused: {reason}')
        vendor_length = struct.unpack_from('<H', data, 16)[0]
        formats = data[21]
        offset = 32 + (vendor_length + 3) // 4 * 4 + 8 * formats
        self.vendor = data[32:32 + vendor_length].decode(errors='replace')
        self.release, self.resource_base = struct.unpack_from('<II', data, 0)
        self.root = struct.unpack_from('<I', data, offset)[0]
        self.width, self.height = struct.unpack_from('<HH', data, offset + 20)

    def request(self, opcode, detail, body=b''):
        body += b'\0' * (-len(body) % 4)
        self.sock.sendall(struct.pack('<BBH', opcode, detail, 1 + len(body) // 4) + body)
        while True:
            header = self._read(32)
            if header[0] == 0:
                raise XError(f'X error {header[1]} for request {opcode}')
            if header[0] == 1:
                extra = struct.unpack_from('<I', header, 4)[0] * 4
                return header + self._read(extra)
            # No events are selected; skip anything else defensively.

    def query_extension(self, name):
        raw = name.encode()
        reply = self.request(98, 0, struct.pack('<HH', len(raw), 0) + raw)
        return bool(reply[8]), reply[9]

    def extensions(self, names=X_EXTENSIONS):
        return {name: self.query_extension(name)[0] for name in names}

    def glx_version(self):
        present, major = self.query_extension('GLX')
        if not present:
            return None
        reply = self.request(major, 7, struct.pack('<II', 1, 4))  # glXQueryVersion
        return list(struct.unpack_from('<II', reply, 8))

    def children(self, window):
        reply = self.request(15, 0, struct.pack('<I', window))
        count = struct.unpack_from('<H', reply, 16)[0]
        return list(struct.unpack_from(f'<{count}I', reply, 32))

    def viewable(self, window):
        reply = self.request(3, 0, struct.pack('<I', window))
        return reply[26] == 2, bool(reply[27])  # (IsViewable, override-redirect)

    def string_property(self, window, atom, kind=0, length=64):
        reply = self.request(20, 0, struct.pack('<IIIII', window, atom, kind, 0, length))
        size = struct.unpack_from('<I', reply, 16)[0] * (reply[1] // 8)
        return reply[32:32 + size].decode(errors='replace')

    def top_level_windows(self):
        """Viewable, managed top-level windows: [(id, wm_class, title)]."""
        result = []
        for window in self.children(self.root):
            try:
                viewable, override = self.viewable(window)
                if not viewable or override:
                    continue
                wm_class = self.string_property(window, self.WM_CLASS, self.STRING).split('\0')
                title = self.string_property(window, self.WM_NAME)
            except XError:
                continue  # destroyed while we looked
            result.append((window, [part for part in wm_class if part], title[:80]))
        return result


def is_steam_window(wm_class):
    return any('steam' in part.lower() for part in wm_class)


def describe_display(display):
    connection = XConnection(display)
    try:
        return dict(display=display, socket=connection.path, vendor=connection.vendor,
                    release=connection.release, root_size=[connection.width, connection.height],
                    extensions=connection.extensions(), glx_version=connection.glx_version())
    finally:
        connection.close()


def wait_for_display(display, deadline_s, child=None):
    deadline = time.monotonic() + deadline_s
    last = None
    while time.monotonic() < deadline:
        if child is not None and child.poll() is not None:
            raise RuntimeError(f'X server for {display} exited with {child.returncode}')
        try:
            return describe_display(display)
        except (OSError, XError) as error:
            last = error
        time.sleep(0.25)
    raise RuntimeError(f'X display {display} is not reachable: {last}')


# ---------------------------------------------------------------------------
# GLX probe, run in a child so libGL never enters the launcher process.
# ---------------------------------------------------------------------------
def full_python():
    """A Python with ctypes, found at runtime: BROOK_STEAM_FULL_PYTHON (a path,
    or 'none'), else an installed /nix/store python3 3.x (not -minimal) whose
    lib-dynload has _ctypes and which imports it."""
    override = os.environ.get('BROOK_STEAM_FULL_PYTHON')
    if override == 'none':
        return None
    if override:
        candidates = [override]
    else:
        same = f'python3-{sys.version_info.major}.{sys.version_info.minor}.'
        found = [path for path in glob.glob('/nix/store/*-python3-3.*/bin/python3') if 'minimal' not in path]
        candidates = sorted(found, key=lambda path: (same not in path, path.endswith('-env/bin/python3'), path))
    for candidate in candidates:
        if not os.access(candidate, os.X_OK):
            continue
        if not override and not glob.glob(str(Path(candidate).parent.parent / 'lib/python3.*/lib-dynload/_ctypes*')):
            continue
        try:
            if subprocess.run([candidate, '-c', 'import ctypes'], capture_output=True, timeout=15).returncode == 0:
                return candidate
        except (OSError, subprocess.TimeoutExpired):
            continue
    return None


def loaded_libraries(pattern):
    """Shared objects in this process (Brook's /proc/self/maps omits files)."""
    class Info(ctypes.Structure):
        _fields_ = [('addr', ctypes.c_void_p), ('name', ctypes.c_char_p)]
    names = []
    callback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(Info), ctypes.c_size_t, ctypes.c_void_p)(
        lambda info, size, data: names.append((info.contents.name or b'').decode(errors='replace')) or 0)
    ctypes.CDLL(None).dl_iterate_phdr(callback, None)
    return sorted({os.path.realpath(name) for name in names if re.search(pattern, name)})


def glx_probe(display):
    if ctypes is None:
        print(json.dumps(dict(ok=False, error=f'{sys.executable} has no ctypes; set BROOK_STEAM_FULL_PYTHON')))
        return 1
    signal.alarm(30)
    os.environ['DISPLAY'] = display
    os.environ.setdefault('__GLX_VENDOR_LIBRARY_NAME', 'mesa')

    def load(*names):
        for name in names:
            try:
                return ctypes.CDLL(name, mode=ctypes.RTLD_GLOBAL)
            except OSError:
                continue
        raise OSError(f'cannot load any of {names}')

    X = load('/nix/profile/lib/libX11.so.6', 'libX11.so.6')
    for vendor in ('/nix/profile/lib/libGLX_mesa.so.0', 'libGLX_mesa.so.0'):
        try:
            ctypes.CDLL(vendor, mode=ctypes.RTLD_GLOBAL)
            break
        except OSError:
            pass
    G = load('/nix/profile/lib/libGL.so.1', 'libGL.so.1')
    P, I, U = ctypes.c_void_p, ctypes.c_int, ctypes.c_ulong

    def fn(lib, name, result, argtypes):
        f = getattr(lib, name)
        f.restype, f.argtypes = result, argtypes
        return f

    errors = []

    class Err(ctypes.Structure):
        _fields_ = [('type', I), ('display', P), ('resource', U), ('serial', U),
                    ('error', ctypes.c_ubyte), ('request', ctypes.c_ubyte), ('minor', ctypes.c_ubyte)]

    @ctypes.CFUNCTYPE(I, P, ctypes.POINTER(Err))
    def on_error(_, event):
        errors.append(dict(error=event.contents.error, request=event.contents.request,
                           minor=event.contents.minor))
        return 0

    fn(X, 'XSetErrorHandler', P, [P])(ctypes.cast(on_error, P))
    d = fn(X, 'XOpenDisplay', P, [ctypes.c_char_p])(display.encode())
    if not d:
        print(json.dumps(dict(ok=False, error='XOpenDisplay failed', display=display)))
        return 1
    screen = fn(X, 'XDefaultScreen', I, [P])(d)
    result = dict(display=display, errors=errors,
                  software_requested=os.environ.get('LIBGL_ALWAYS_SOFTWARE') == '1')
    try:
        a, b = I(), I()
        result['glx'] = bool(fn(G, 'glXQueryExtension', I, [P, ctypes.POINTER(I), ctypes.POINTER(I)])(d, ctypes.byref(a), ctypes.byref(b)))
        fn(G, 'glXQueryVersion', I, [P, ctypes.POINTER(I), ctypes.POINTER(I)])(d, ctypes.byref(a), ctypes.byref(b))
        result['glx_version'] = [a.value, b.value]
        vendor = fn(G, 'glXQueryServerString', ctypes.c_char_p, [P, I, I])(d, screen, 1)
        result['glx_server_vendor'] = vendor.decode() if vendor else None
        attrs = (I * 11)(4, 8, 8, 9, 8, 10, 8, 12, 24, 0, 0)  # RGBA, DOUBLEBUFFER, depth 24
        vi = fn(G, 'glXChooseVisual', P, [P, I, ctypes.POINTER(I)])(d, screen, attrs)
        result['visual'] = bool(vi)
        if vi:
            class Visual(ctypes.Structure):
                _fields_ = [('visual', P), ('id', U), ('screen', I), ('depth', I), ('klass', I),
                            ('red', U), ('green', U), ('blue', U), ('size', I), ('bits', I)]

            class Attr(ctypes.Structure):
                _fields_ = [('bgmap', U), ('bg', U), ('bordermap', U), ('border', U), ('bitgravity', I),
                            ('wingravity', I), ('backing', I), ('planes', U), ('pixel', U), ('save', I),
                            ('events', ctypes.c_long), ('propagate', ctypes.c_long), ('override', I),
                            ('colormap', U), ('cursor', U)]
            visual = ctypes.cast(vi, ctypes.POINTER(Visual)).contents
            root = fn(X, 'XRootWindow', U, [P, I])(d, screen)
            cmap = fn(X, 'XCreateColormap', U, [P, U, P, I])(d, root, visual.visual, 0)
            attr = Attr()
            attr.colormap = cmap
            win = fn(X, 'XCreateWindow', U, [P, U, I, I, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, I,
                                             ctypes.c_uint, P, U, P])(
                d, root, 0, 0, 64, 64, 0, visual.depth, 1, visual.visual, (1 << 3) | (1 << 13), ctypes.byref(attr))
            fn(X, 'XStoreName', I, [P, U, ctypes.c_char_p])(d, win, b'Brook GLX probe')
            fn(X, 'XMapWindow', I, [P, U])(d, win)
            fn(X, 'XSync', I, [P, I])(d, 0)
            ctx = fn(G, 'glXCreateContext', P, [P, P, P, I])(d, vi, None, 1)
            result['context'] = bool(ctx)
            if ctx and fn(G, 'glXMakeCurrent', I, [P, U, P])(d, win, ctx):
                result['direct'] = bool(fn(G, 'glXIsDirect', I, [P, P])(d, ctx))
                get = fn(G, 'glGetString', ctypes.c_char_p, [ctypes.c_uint])
                result['vendor'] = (get(0x1F00) or b'').decode()
                result['renderer'] = (get(0x1F01) or b'').decode()
                result['gl_version'] = (get(0x1F02) or b'').decode()
                fn(G, 'glClearColor', None, [ctypes.c_float] * 4)(1, 0, 0, 1)
                fn(G, 'glClear', None, [ctypes.c_uint])(0x4000)
                fn(G, 'glFinish', None, [])()
                pixel = (ctypes.c_ubyte * 4)()
                fn(G, 'glReadPixels', None, [I, I, I, I, ctypes.c_uint, ctypes.c_uint, P])(0, 0, 1, 1, 0x1908, 0x1401, pixel)
                result['pixel'] = list(pixel)
                result['gl_error'] = fn(G, 'glGetError', ctypes.c_uint, [])()
                fn(G, 'glXMakeCurrent', I, [P, U, P])(d, 0, None)
            if ctx:
                fn(G, 'glXDestroyContext', None, [P, P])(d, ctx)
            fn(X, 'XDestroyWindow', I, [P, U])(d, win)
            fn(X, 'XFreeColormap', I, [P, U])(d, cmap)
            fn(X, 'XFree', I, [P])(vi)
        fn(X, 'XSync', I, [P, I])(d, 0)
    finally:
        fn(X, 'XCloseDisplay', I, [P])(d)
    result['gl_libraries'] = loaded_libraries(r'/lib(gallium|GLX_mesa|GLX|GL|EGL_mesa)[-.]')
    result['hardware'] = bool(result.get('renderer')) and 'llvmpipe' not in result['renderer'] \
        and 'softpipe' not in result['renderer']
    result['ok'] = result.get('pixel') == [255, 0, 0, 255] and not errors and result.get('gl_error') == 0
    print(json.dumps(result), flush=True)
    return 0 if result['ok'] else 1


# ---------------------------------------------------------------------------
# Process ownership
# ---------------------------------------------------------------------------
def read_status(pid):
    fields = {}
    try:
        with open(f'/proc/{pid}/status') as stream:
            for line in stream:
                key, _, value = line.partition(':')
                fields[key] = value.strip()
    except OSError:
        return None
    return fields


def cmdline(pid):
    try:
        with open(f'/proc/{pid}/cmdline', 'rb') as stream:
            return stream.read().split(b'\0')
    except OSError:
        return []


def process_table():
    """{pid: (ppid, state)} for every thread-group leader."""
    table = {}
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        status = read_status(entry)
        if not status:
            continue
        try:
            pid, tgid, ppid = int(status['Pid']), int(status.get('Tgid', status['Pid'])), int(status['PPid'])
        except (KeyError, ValueError):
            continue
        if pid == tgid:
            table[pid] = (ppid, status.get('State', '?')[:1])
    return table


def descendants(root, exclude=()):
    table = process_table()
    children = {}
    for pid, (ppid, _) in table.items():
        children.setdefault(ppid, []).append(pid)
    found, pending = {}, list(children.get(root, []))
    while pending:
        pid = pending.pop()
        if pid in exclude or pid in found:
            continue
        found[pid] = table[pid][1]
        pending += children.get(pid, [])
    return found


def reap_orphans(tracked):
    """Reap adopted descendants (we are a child subreaper) without touching
    the processes whose exit status subprocess.Popen still owns."""
    for pid, (ppid, state) in process_table().items():
        if ppid == os.getpid() and state == 'Z' and pid not in tracked:
            try:
                os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                pass


def signal_all(pids, sig):
    for pid in pids:
        try:
            os.kill(pid, sig)
        except (ProcessLookupError, PermissionError):
            pass


def set_child_subreaper():
    if ctypes is None:
        return False
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        return libc.prctl(36, 1, 0, 0, 0) == 0  # PR_SET_CHILD_SUBREAPER
    except (OSError, AttributeError):
        return False


def stop_popen(process, term_s=3.0):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=term_s)
    except subprocess.TimeoutExpired:
        process.kill()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass


def keeper(watched, command):
    """Run one command and stop it when the launcher disappears. Brook does
    not implement PR_SET_PDEATHSIG, so poll the parent instead."""
    stop = []
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, lambda *_: stop.append(1))
    child = subprocess.Popen(command)
    while child.poll() is None:
        if stop or os.getppid() != watched:
            stop_popen(child, 3)
            return 0
        time.sleep(0.5)
    return child.returncode


def acquire_instance():
    try:
        previous = int(PIDFILE.read_text().strip())
    except (OSError, ValueError):
        previous = None
    if previous and previous != os.getpid():
        if any(b'brook-steam' in part or b'brook_steam_launcher' in part for part in cmdline(previous)[:2]):
            return previous
    PIDFILE.write_text(f'{os.getpid()}\n')
    return None


def release_instance():
    try:
        if PIDFILE.read_text().strip() == str(os.getpid()):
            PIDFILE.unlink()
    except OSError:
        pass


def write_state(**state):
    try:
        STATE.write_text(json.dumps(dict(state, updated=time.time()), indent=2, default=str) + '\n')
    except OSError:
        pass


# ---------------------------------------------------------------------------
# PulseAudio
# ---------------------------------------------------------------------------
def socket_accepts(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
        probe.settimeout(0.5)
        try:
            probe.connect(str(path))
            return True
        except OSError:
            return False


class PulseServer:
    def __init__(self, runtime_base, output):
        self.binary = shutil.which('pulseaudio', path='/nix/profile/bin:' + os.environ.get('PATH', ''))
        if not self.binary:
            raise RuntimeError('Steam needs pulseaudio in /nix/profile/bin (or pass --pulse-server/--no-pulse)')
        runtime_base.mkdir(mode=0o700, parents=True, exist_ok=True)
        # Remove directories left by launchers that no longer exist.
        for stale in runtime_base.glob(PULSE_PREFIX + '*'):
            pid = stale.name[len(PULSE_PREFIX):]
            if pid.isdigit() and not Path(f'/proc/{pid}').exists() and stale.stat().st_uid == os.getuid():
                shutil.rmtree(stale, ignore_errors=True)
        self.directory = runtime_base / f'{PULSE_PREFIX}{os.getpid()}'
        shutil.rmtree(self.directory, ignore_errors=True)
        self.directory.mkdir(mode=0o700)
        self.socket = self.directory / 'native'
        self.server = f'unix:{self.socket}'
        command = [self.binary, '-n', '--daemonize=no', '--exit-idle-time=-1', '--use-pid-file=no',
                   '--log-target=stderr',
                   '-L', f'module-native-protocol-unix socket={self.socket} auth-anonymous=1',
                   '-L', 'module-null-sink sink_name=brook_null',
                   '-L', 'module-always-sink']
        env = dict(os.environ, XDG_RUNTIME_DIR=str(self.directory), HOME=str(HOME))
        for key in ('PULSE_SERVER', 'DBUS_SESSION_BUS_ADDRESS'):
            env.pop(key, None)  # private server: no session-bus registration
        self.process = subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), '--_keeper', str(os.getpid()), '--', *command],
            env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=output, start_new_session=True)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.stop()
                raise RuntimeError(f'pulseaudio exited with {self.process.returncode}; see {LOG}')
            if socket_accepts(self.socket):
                break
            time.sleep(0.1)
        else:
            self.stop()
            raise RuntimeError('pulseaudio did not open its socket within 10 s')
        self.info = self._info()
        log('pulse ready', server=self.server, keeper=self.process.pid, info=self.info)

    def _info(self):
        pactl = shutil.which('pactl', path='/nix/profile/bin:' + os.environ.get('PATH', ''))
        if not pactl:
            return None
        try:
            result = subprocess.run([pactl, 'info'], env=dict(os.environ, PULSE_SERVER=self.server),
                                    capture_output=True, text=True, timeout=5)
        except (OSError, subprocess.TimeoutExpired) as error:
            return f'pactl failed: {error}'
        keep = ('Server Name', 'Server Version', 'Default Sink')
        return {k.strip(): v.strip() for k, _, v in (line.partition(':') for line in result.stdout.splitlines())
                if k.strip() in keep} or f'pactl exit {result.returncode}'

    def stop(self):
        stop_popen(self.process, 5)
        shutil.rmtree(self.directory, ignore_errors=True)


# ---------------------------------------------------------------------------
# Graphics environment
# ---------------------------------------------------------------------------
def patched_iris():
    """Library and EGL vendor directories of the opt-in patched Iris build,
    read from its brook-iris-run wrapper."""
    candidates = [os.environ.get('BROOK_IRIS_RUN'), '/nix/profile/bin/brook-iris-run',
                  *sorted(glob.glob('/nix/store/*-mesa-*/bin/brook-iris-run'))]
    for candidate in candidates:
        if not candidate or not os.path.isfile(candidate):
            continue
        text = Path(candidate).read_text(errors='replace')
        libs = re.search(r'LD_LIBRARY_PATH="(/nix/store/[^/"]+/lib/brook-iris)', text)
        egl = re.search(r'__EGL_VENDOR_LIBRARY_DIRS=(/nix/store/\S+)', text)
        if libs and Path(libs.group(1)).is_dir():
            return dict(wrapper=candidate, library_dir=libs.group(1), egl_vendor_dir=egl.group(1) if egl else None)
    raise RuntimeError('--iris needs brook-iris-run (patched Iris); none found')


def graphics_environment(env, gl, iris, gl_debug):
    env.pop('LIBGL_ALWAYS_SOFTWARE', None)
    extra = {}
    if gl == 'software':
        env['LIBGL_ALWAYS_SOFTWARE'] = '1'
    if iris:
        extra = patched_iris()
        env['LD_LIBRARY_PATH'] = extra['library_dir'] + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
        if extra['egl_vendor_dir']:
            env.pop('__EGL_VENDOR_LIBRARY_FILENAMES', None)
            env['__EGL_VENDOR_LIBRARY_DIRS'] = extra['egl_vendor_dir']
    if gl_debug:
        env['LIBGL_DEBUG'] = 'verbose'
    return extra


def run_glx_probe(display, env, timeout=40):
    python = sys.executable if ctypes is not None else full_python()
    if python is None:
        return dict(ok=False, error='GLX probe needs a Python with ctypes; none found (set BROOK_STEAM_FULL_PYTHON)')
    try:
        result = subprocess.run([python, os.path.abspath(__file__), '--_glx-probe', display],
                                env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return dict(ok=False, error=f'GLX probe timed out after {timeout} s')
    for line in reversed(result.stdout.splitlines()):
        if line.startswith('{'):
            report = json.loads(line)
            break
    else:
        report = dict(ok=False, error=f'GLX probe exit {result.returncode}')
    if result.stderr.strip():
        report['stderr_tail'] = result.stderr.strip()[-1500:]
    return report


# ---------------------------------------------------------------------------
# Launcher
# ---------------------------------------------------------------------------
def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true', help='verify prerequisites and the target display, then exit')
    parser.add_argument('--probe', action='store_true', help='--check plus a GLX context/renderer probe on the display')
    parser.add_argument('--xephyr', action='store_true', help='fallback: nested software Xephyr :14 (previous launcher)')
    parser.add_argument('--display', help='rootless target display (default: $DISPLAY, else :0)')
    parser.add_argument('--gl', choices=('gpu', 'software'), default='gpu',
                        help='rootless OpenGL: hardware through the desktop Xwayland (default) or llvmpipe')
    parser.add_argument('--iris', action='store_true',
                        help='experimental: prefer the patched Iris libgallium from brook-iris-run')
    parser.add_argument('--gl-debug', action='store_true', help='set LIBGL_DEBUG=verbose for the client and games')
    parser.add_argument('--allow-no-glx', action='store_true', help='start even if the display lacks GLX')
    parser.add_argument('--pulse-server', help='use this existing unix:/path PulseAudio server instead of owning one')
    parser.add_argument('--no-pulse', action='store_true', help='start no audio server (libpulse may autospawn-loop)')
    parser.add_argument('--windowless-grace', type=float, default=20,
                        help='end Steam this many seconds after its last window closed with no game running; 0 disables')
    parser.add_argument('--startup-timeout', type=float, default=900,
                        help='end Steam if it shows no window within this many seconds; 0 disables')
    parser.add_argument('--launch-app', type=int, help='ask Steam to launch this installed AppID')
    parser.add_argument('--_keeper', type=int, help=argparse.SUPPRESS)
    parser.add_argument('--_glx-probe', help=argparse.SUPPRESS)
    parser.add_argument('command', nargs='*', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.command and args._keeper is None:
        parser.error(f'unexpected arguments: {args.command}')
    if args.xephyr and (args.gl != 'gpu' or args.iris or args.display):
        parser.error('--xephyr always uses software GL on its own :14 display')
    if args.pulse_server and (args.no_pulse or not args.pulse_server.startswith('unix:/')):
        parser.error('--pulse-server needs an absolute unix:/path and excludes --no-pulse')
    return args


class Steam:
    def __init__(self, args):
        self.args = args
        self.install = Path(os.environ.get('BROOK_STEAM_INSTALL', HOME / 'steam-20261022'))
        self.client = self.install / 'client'
        self.provider = Path(os.environ.get('BROOK_STEAM_PROVIDER', BROOK / 'steam-provider-20261103'))
        self.helper = Path(os.environ.get('BROOK_STEAM_HELPER',
                                          Path(os.path.abspath(__file__)).parent / 'steam-client-check.py'))
        self.bwrap = self.client / ('steamrt64/pv-runtime/steam-runtime-steamrt/'
                                    'pressure-vessel/libexec/steam-runtime-tools-0/srt-bwrap')
        self.loader = self.provider / 'ld-linux-x86-64.so.2'
        self.xephyr = None
        self.pulse = None
        self.server = None
        self.main = None
        self.stop_requested = None
        self.display = None
        self.report = {}
        self.subreaper = False
        self.seen = {}  # pid -> cmdline, for descendants that leave our tree without a subreaper

    def client_processes(self, include_services=False):
        """Live descendants. Without a child subreaper an orphaned descendant is
        reparented to init, so also keep every process seen before whose
        command line is unchanged (a PID-reuse guard)."""
        found = descendants(os.getpid(), () if include_services else self.owned_services())
        if self.subreaper:
            return found
        for pid in found:
            self.seen.setdefault(pid, cmdline(pid))
        services = set() if include_services else self.owned_services()
        for pid, argv in list(self.seen.items()):
            if pid in found or pid in services:
                continue
            state = (read_status(pid) or {}).get('State', 'Z')[:1]
            if state == 'Z' or cmdline(pid) != argv:
                del self.seen[pid]
            else:
                found[pid] = state
        return found

    def prerequisites(self):
        needed = [self.bwrap, self.loader, self.client / 'steamrt64/steam']
        if self.args.xephyr:
            self.xephyr = Path('/nix/profile/bin/Xephyr')
            if not os.access(self.xephyr, os.X_OK):
                candidates = sorted(glob.glob('/nix/store/*-xorg-server-*/bin/Xephyr'))
                if len(candidates) != 1:
                    raise RuntimeError('Steam --xephyr needs one installed Xephyr executable')
                self.xephyr = Path(candidates[0])
            needed.append(self.xephyr)
        for path in needed:
            if not os.access(path, os.X_OK):
                raise RuntimeError(f'Steam needs {path}')
        for path in (self.helper, self.provider / 'ld.so.cache', RESOLV):
            if not path.is_file():
                raise RuntimeError(f'Steam needs {path}')
        if not (self.args.no_pulse or self.args.pulse_server):
            if not shutil.which('pulseaudio', path='/nix/profile/bin:' + os.environ.get('PATH', '')):
                raise RuntimeError('Steam needs pulseaudio in /nix/profile/bin (or pass --pulse-server/--no-pulse)')

    def target_display(self):
        return ':14' if self.args.xephyr else (self.args.display or os.environ.get('DISPLAY') or ':0')

    def check_display(self, report):
        if not report['extensions'].get('GLX') and not self.args.allow_no_glx:
            raise RuntimeError(f'{report["display"]} has no GLX extension; use --xephyr (software) or --allow-no-glx')

    def environment(self):
        env = dict(os.environ)
        extra = {}
        if self.args.xephyr:
            env.update(DISPLAY=':0', LIBGL_ALWAYS_SOFTWARE='1')  # Xephyr itself is a :0 client
        else:
            env['DISPLAY'] = self.display
            extra = graphics_environment(env, self.args.gl, self.args.iris, self.args.gl_debug)
        return env, extra

    def command(self, extra):
        etc = BROOK / 'steam-private-etc'
        etc.mkdir(mode=0o700, exist_ok=True)
        (etc / 'passwd').write_text('brook:x:1000:100:Brook:/data/home:/nix/profile/bin/sh\n')
        (etc / 'group').write_text('users:x:100:brook\n')
        command = [str(self.bwrap), '--tmpfs', '/']
        for mount in ('/nix', '/data', '/tmp', '/proc', '/boot', '/usr', '/sys'):
            command += ['--dir', mount, '--bind', mount, mount]
        command += ['--dev-bind', '/dev', '/dev']
        for path, target in ((etc / 'passwd', '/etc/passwd'), (etc / 'group', '/etc/group'),
                             (self.provider / 'ld.so.cache', '/etc/ld.so.cache'),
                             (self.provider / 'ld.so.cache', '/var/cache/ldconfig/ld.so.cache'),
                             (RESOLV, '/etc/resolv.conf'),
                             (self.loader, '/lib64/ld-linux-x86-64.so.2'),
                             ('/nix/profile/bin/sh', '/bin/sh'),
                             ('/nix/profile/bin/bash', '/bin/bash')):
            command += ['--bind', str(path), target]
        command += ['--', '/nix/profile/bin/python3', str(self.helper), '--client', str(self.client),
                    '--home', str(self.install / 'user'), '--log-dir', str(HOME / 'steamrt-evidence'),
                    '--skip-bootstrap', '--display', self.display]
        if self.pulse_server:
            command += ['--pulse-server', self.pulse_server]
        if extra.get('library_dir'):
            command += ['--library-dir', extra['library_dir']]
        if self.args.launch_app is not None:
            command += ['--launch-app', str(self.args.launch_app)]
        return command

    @property
    def pulse_server(self):
        return self.pulse.server if self.pulse else self.args.pulse_server

    def start_xephyr(self, env, output):
        address = '/tmp/.X11-unix/X14'
        if os.path.lexists(address) or os.path.lexists('/tmp/.X14-lock'):
            if socket_accepts(address):
                raise RuntimeError('Steam’s nested display :14 is already in use')
            os.unlink(address)  # left by a server that no longer exists
        # -displayfd also skips the X lock file, whose link() Brook's /tmp
        # once lacked; -nolock is root-only.
        ready_read, ready_write = os.pipe()
        self.server = subprocess.Popen(
            [str(self.xephyr), ':14', '-displayfd', str(ready_write), '-screen', '1280x800',
             '-resizeable', '-nolisten', 'tcp'],
            env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=output, pass_fds=(ready_write,),
            start_new_session=True)
        os.close(ready_write)
        self._ready_read = ready_read  # keep open so the server's write succeeds
        return wait_for_display(':14', 12, self.server)

    def steam_main_processes(self):
        pids = self.client_processes()
        found = [pid for pid in pids if cmdline(pid)[:1] and cmdline(pid)[0].endswith(b'/steamrt64/steam')]
        return found or [pid for pid in pids if (read_status(pid) or {}).get('Name') == 'steam']

    def owned_services(self):
        excluded = set()
        for process in (self.pulse.process if self.pulse else None, self.server):
            if process is not None and process.poll() is None:
                excluded.add(process.pid)
        return excluded

    def game_running(self):
        return any(any(part == b'SteamLaunch' for part in cmdline(pid))
                   for pid in self.client_processes())

    def tracked(self):
        return {p.pid for p in (self.main, self.server, self.pulse.process if self.pulse else None) if p}

    def supervise(self):
        args = self.args
        started = time.monotonic()
        seen_window, windowless_since, connection, last_windows = False, None, None, None
        next_check = 0.0
        while True:
            if self.stop_requested:
                return f'signal {self.stop_requested}'
            if self.main.poll() is not None:
                return f'client exited {self.main.returncode}'
            if self.server is not None and self.server.poll() is not None:
                return f'Xephyr exited {self.server.returncode}'
            now = time.monotonic()
            if now >= next_check:
                next_check = now + 2
                reap_orphans(self.tracked())
                if not self.subreaper:
                    self.client_processes()  # remember descendants while they are still in our tree
                try:
                    if connection is None:
                        connection = XConnection(self.display)
                    windows = connection.top_level_windows()
                except (OSError, XError) as error:
                    if connection is not None:
                        log('display connection lost', display=self.display, error=str(error))
                        connection.close()
                    connection, windows = None, None
                if windows is not None:
                    steam_windows = [w for w in windows if is_steam_window(w[1])]
                    summary = sorted((w[2] or '/'.join(w[1]))[:40] for w in steam_windows)
                    if summary != last_windows:
                        log('steam windows', windows=summary)
                        last_windows = summary
                    if steam_windows:
                        if not seen_window:
                            log('first steam window', after_s=round(now - started, 1))
                        seen_window, windowless_since = True, None
                    elif seen_window and args.windowless_grace > 0:
                        if self.game_running():
                            windowless_since = None
                        elif windowless_since is None:
                            windowless_since = now
                        elif now - windowless_since >= args.windowless_grace:
                            return f'no Steam window for {args.windowless_grace:g} s'
                if not seen_window and args.startup_timeout > 0 and now - started > args.startup_timeout:
                    return f'no Steam window within {args.startup_timeout:g} s'
            time.sleep(0.5)

    def wait_main(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.main.poll() is not None:
                return True
            reap_orphans(self.tracked())
            time.sleep(0.2)
        return self.main.poll() is not None

    def stop_client(self, reason):
        """Bounded: about 15 s for Steam to exit, then 5 s TERM and 3 s KILL."""
        if self.main is not None and self.main.poll() is None:
            steam = self.steam_main_processes()
            log('stopping steam', reason=reason, steam_pids=steam)
            # Steam handles SIGTERM as a normal shutdown; its runtime
            # supervisor then ends the remaining client subtree.
            signal_all(steam, signal.SIGTERM)
            if not steam:
                signal_all([self.main.pid], signal.SIGTERM)
            self.wait_main(15)
        remaining = self.client_processes()
        if remaining:
            log('terminating leftover client processes', count=len(remaining))
            signal_all(remaining, signal.SIGTERM)
            deadline = time.monotonic() + 5
            while remaining and time.monotonic() < deadline:
                reap_orphans(self.tracked())
                time.sleep(0.2)
                remaining = {p: s for p, s in self.client_processes().items() if s != 'Z'}
        if remaining:
            log('killing leftover client processes', pids=sorted(remaining))
            signal_all(remaining, signal.SIGKILL)
        if self.main is not None and self.main.poll() is None:
            self.main.kill()
        if self.main is not None:
            try:
                self.main.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass

    def shutdown_services(self):
        if self.server is not None:
            stop_popen(self.server, 5)
            os.close(self._ready_read)
        if self.pulse is not None:
            self.pulse.stop()
        # Anything still left (for example a server whose keeper was killed
        # and that we adopted as subreaper) goes now.
        remaining = self.client_processes(include_services=True)
        if remaining:
            signal_all(remaining, signal.SIGKILL)
            time.sleep(0.2)
        reap_orphans(set())

    def run(self, output):
        args = self.args
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(sig, lambda number, _: setattr(self, 'stop_requested', signal.Signals(number).name))
        subreaper = self.subreaper = set_child_subreaper()
        self.display = self.target_display()
        log('start', version=LAUNCHER_VERSION, mode='xephyr' if args.xephyr else 'rootless',
            display=self.display, gl='software' if args.xephyr else args.gl, iris=args.iris,
            subreaper=subreaper, pid=os.getpid(), python=sys.executable)
        if not subreaper:
            log('no child subreaper: descendants whose parent exits are tracked by PID and command line')
        reason, code = 'setup failed', 1
        try:
            if not (args.no_pulse or args.pulse_server):
                self.pulse = PulseServer(Path(os.environ.get('XDG_RUNTIME_DIR') or '/tmp'), output)
            env, extra = self.environment()
            if args.xephyr:
                self.report = self.start_xephyr(env, output)
            else:
                self.report = wait_for_display(self.display, 30)
                self.check_display(self.report)
            log('display ready', **self.report)
            command = self.command(extra)
            child_env = dict(env, LD_LIBRARY_PATH=':'.join(
                str(self.client / p) for p in ('hostlibs', 'steamrt64', 'ubuntu12_64')))
            if self.stop_requested:
                raise RuntimeError(f'stopped by {self.stop_requested} before the client started')
            self.main = subprocess.Popen(command, env=child_env, stdin=subprocess.DEVNULL, stdout=output,
                                         stderr=output, start_new_session=True)
            write_state(pid=os.getpid(), version=LAUNCHER_VERSION, mode='xephyr' if args.xephyr else 'rootless',
                        display=self.display, display_report=self.report, pulse_server=self.pulse_server,
                        pulse_keeper=self.pulse.process.pid if self.pulse else None, client_pid=self.main.pid,
                        graphics=dict(gl='software' if args.xephyr else args.gl, **extra),
                        state='running')
            log('client started', pid=self.main.pid, pulse_server=self.pulse_server)
            reason = self.supervise()
        except (OSError, RuntimeError, XError) as error:
            reason = f'error: {error}'
            print(f'Steam: {error}', file=sys.stderr)
        finally:
            started = time.monotonic()
            self.stop_client(reason)
            self.shutdown_services()
            # A client that exits by itself reports its own status; ending a
            # windowless or signalled Steam is the launcher's normal outcome.
            if reason.startswith('client exited'):
                code = self.main.returncode
            elif not reason.startswith(('error', 'setup', 'Xephyr')):
                code = 0
            leftovers = self.client_processes(include_services=True)
            write_state(pid=os.getpid(), version=LAUNCHER_VERSION, state='stopped', reason=reason,
                        exit=code, cleanup_s=round(time.monotonic() - started, 1), leftovers=sorted(leftovers))
            log('stopped', reason=reason, exit=code, cleanup_s=round(time.monotonic() - started, 1),
                leftovers=sorted(leftovers))
        return code


def main(argv=None):
    global _log_stream
    args = parse_arguments(sys.argv[1:] if argv is None else argv)
    if args._keeper is not None:
        return keeper(args._keeper, args.command)
    if args._glx_probe:
        return glx_probe(args._glx_probe)
    if ctypes is None and not os.environ.get('BROOK_STEAM_REEXEC'):
        # Re-run under a full Python so the launcher can be a child subreaper.
        python = full_python()
        if python:
            argv = sys.argv[1:] if argv is None else argv
            os.execve(python, [python, os.path.abspath(__file__), *argv], dict(os.environ, BROOK_STEAM_REEXEC='1'))
    if os.geteuid() == 0:
        raise RuntimeError('run Steam as the desktop user, not root')
    steam = Steam(args)
    steam.prerequisites()
    if args.check or args.probe:
        report = dict(prerequisites='present', version=LAUNCHER_VERSION,
                      mode='xephyr' if args.xephyr else 'rootless')
        if not args.xephyr:
            steam.display = steam.target_display()
            report['display'] = describe_display(steam.display)
            env, extra = steam.environment()
            report['graphics'] = dict(gl=args.gl, **extra)
            if args.probe:
                report['glx_probe'] = run_glx_probe(steam.display, env)
        print(json.dumps(report, indent=2))
        if not args.xephyr:
            steam.check_display(report['display'])
            if args.probe and not report['glx_probe'].get('ok'):
                return 1
        print('Steam launcher prerequisites present')
        return 0
    BROOK.mkdir(parents=True, exist_ok=True)
    running = acquire_instance()
    if running:
        log('already running; not starting another client', pid=running)
        return 0
    try:
        with LOG.open('a') as output:
            _log_stream = output
            return steam.run(output)
    finally:
        _log_stream = None
        release_instance()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, XError) as error:
        print(f'Steam: {error}', file=sys.stderr)
        # The desktop launcher discards stderr; keep the reason with the log.
        log(f'Steam: {error}')
        sys.exit(1)

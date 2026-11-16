#!/usr/bin/env python3
"""X11 HiDPI / geometry probe for Brook's rootless Xwayland (guest only).

Never uses the host display: it requires an explicit --display and refuses to
run when WAYLAND_DISPLAY or DISPLAY point at a host session it was not given.
Prints JSON lines.

  info                 Xft.dpi, XSETTINGS owner, screen pixels/mm, EWMH support
  window [--fullscreen] [--class NAME] [--size WxH]
                       Xlib window with a calibration pattern; reports every
                       ConfigureNotify and, per button press, the event's
                       window/root coordinates and XQueryPointer's
  spy                  root listener: EWMH client messages, configures, maps
  sdl [--fullscreen]   SDL2 (sdl2-compat) window like MonoGame: per click the
                       event position, SDL_GetGlobalMouseState minus
                       SDL_GetWindowPosition ("monogame"), window size/position
"""
import argparse
import ctypes
import glob
import json
import os
import sys
import time


def out(**fields):
    print(json.dumps(fields), flush=True)


def library(pattern):
    for path in sorted(glob.glob(pattern), reverse=True):
        try:
            return ctypes.CDLL(path)
        except OSError:  # e.g. a 32-bit copy in the same store
            continue
    raise SystemExit(f'no loadable library matches {pattern}')


class XEvent(ctypes.Union):
    _fields_ = [('type', ctypes.c_int), ('pad', ctypes.c_long * 24)]


def xlib():
    x = library('/nix/store/*-libx11-1.*/lib/libX11.so.6')
    P, I, U, L = ctypes.c_void_p, ctypes.c_int, ctypes.c_uint, ctypes.c_ulong
    sig = {
        'XOpenDisplay': (P, [ctypes.c_char_p]), 'XDefaultScreen': (I, [P]), 'XRootWindow': (L, [P, I]),
        'XResourceManagerString': (ctypes.c_char_p, [P]), 'XInternAtom': (L, [P, ctypes.c_char_p, I]),
        'XGetSelectionOwner': (L, [P, L]), 'XDisplayWidth': (I, [P, I]), 'XDisplayHeight': (I, [P, I]),
        'XDisplayWidthMM': (I, [P, I]), 'XDisplayHeightMM': (I, [P, I]),
        'XCreateSimpleWindow': (L, [P, L, I, I, U, U, U, L, L]), 'XMapWindow': (I, [P, L]),
        'XSelectInput': (I, [P, L, ctypes.c_long]), 'XNextEvent': (I, [P, ctypes.POINTER(XEvent)]),
        'XPending': (I, [P]), 'XFlush': (I, [P]), 'XCreateGC': (P, [P, L, L, P]),
        'XSetForeground': (I, [P, P, L]), 'XFillRectangle': (I, [P, L, P, I, I, U, U]),
        'XChangeProperty': (I, [P, L, L, L, I, I, P, I]), 'XStoreName': (I, [P, L, ctypes.c_char_p]),
        'XQueryPointer': (I, [P, L, ctypes.POINTER(L), ctypes.POINTER(L), ctypes.POINTER(I), ctypes.POINTER(I),
                              ctypes.POINTER(I), ctypes.POINTER(I), ctypes.POINTER(U)]),
        'XGetWindowProperty': (I, [P, L, L, ctypes.c_long, ctypes.c_long, I, L, ctypes.POINTER(L), ctypes.POINTER(I),
                                   ctypes.POINTER(L), ctypes.POINTER(L), ctypes.POINTER(P)]),
        'XFree': (I, [P]), 'XGetAtomName': (P, [P, L]),
    }
    for name, (restype, argtypes) in sig.items():
        getattr(x, name).restype = restype
        getattr(x, name).argtypes = argtypes
    return x


def atoms(x, dpy, window, name):
    actual, fmt, count, after, data = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_void_p()
    if x.XGetWindowProperty(dpy, window, x.XInternAtom(dpy, name, 0), 0, 1024, 0, 0, ctypes.byref(actual),
                            ctypes.byref(fmt), ctypes.byref(count), ctypes.byref(after), ctypes.byref(data)) or not data:
        return []
    values = [ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))[i] for i in range(count.value)] if fmt.value == 32 else []
    x.XFree(data)
    return values


def info(x, dpy):
    screen = x.XDefaultScreen(dpy)
    root = x.XRootWindow(dpy, screen)
    resources = (x.XResourceManagerString(dpy) or b'').decode(errors='replace')
    supported = atoms(x, dpy, root, b'_NET_SUPPORTED')
    width, width_mm = x.XDisplayWidth(dpy, screen), x.XDisplayWidthMM(dpy, screen)
    out(kind='info', resources=resources, xsettings_owner=x.XGetSelectionOwner(dpy, x.XInternAtom(dpy, b'_XSETTINGS_S0', 0)),
        screen=[width, x.XDisplayHeight(dpy, screen)], screen_mm=[width_mm, x.XDisplayHeightMM(dpy, screen)],
        core_dpi=round(width * 25.4 / width_mm, 1) if width_mm else None,
        fullscreen_supported=x.XInternAtom(dpy, b'_NET_WM_STATE_FULLSCREEN', 0) in supported)


def window(x, dpy, args):
    screen = x.XDefaultScreen(dpy)
    root = x.XRootWindow(dpy, screen)
    width, height = (int(v) for v in args.size.split('x'))
    win = x.XCreateSimpleWindow(dpy, root, args.position[0], args.position[1], width, height, 0, 0, 0x203040)
    instance = args.window_class.encode()
    value = instance + b'\0' + instance.title() + b'\0'
    x.XChangeProperty(dpy, win, x.XInternAtom(dpy, b'WM_CLASS', 0), 31, 8, 0, value, len(value))  # XA_STRING
    x.XStoreName(dpy, win, args.window_class.encode())
    if args.fullscreen:  # like SDL: set the state on the withdrawn window
        full = (ctypes.c_ulong * 1)(x.XInternAtom(dpy, b'_NET_WM_STATE_FULLSCREEN', 0))
        x.XChangeProperty(dpy, win, x.XInternAtom(dpy, b'_NET_WM_STATE', 0), 4, 32, 0, full, 1)  # XA_ATOM
    # StructureNotify | ButtonPress | Exposure
    x.XSelectInput(dpy, win, (1 << 17) | (1 << 2) | (1 << 15))
    x.XMapWindow(dpy, win)
    x.XFlush(dpy)
    gc = x.XCreateGC(dpy, win, 0, None)
    size = [width, height]

    def paint():
        # 1-device-pixel stripes in the top-left 64x64 (shows 1:1 vs upscaled),
        # 32x32 corner markers: red TL, green TR, blue BL, white BR.
        x.XSetForeground(dpy, gc, 0x203040)
        x.XFillRectangle(dpy, win, gc, 0, 0, size[0], size[1])
        x.XSetForeground(dpy, gc, 0xffffff)
        for i in range(0, 64, 2):
            x.XFillRectangle(dpy, win, gc, 40 + i, 40, 1, 64)
        for colour, cx, cy in ((0xff0000, 0, 0), (0x00ff00, size[0] - 32, 0),
                               (0x0000ff, 0, size[1] - 32), (0xffffff, size[0] - 32, size[1] - 32)):
            x.XSetForeground(dpy, gc, colour)
            x.XFillRectangle(dpy, win, gc, cx, cy, 32, 32)
        x.XFlush(dpy)

    event = XEvent()
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        if not x.XPending(dpy):
            time.sleep(0.02)
            continue
        x.XNextEvent(dpy, ctypes.byref(event))
        raw = ctypes.cast(ctypes.byref(event), ctypes.POINTER(ctypes.c_int))
        longs = ctypes.cast(ctypes.byref(event), ctypes.POINTER(ctypes.c_long))
        kind = event.type
        if kind == 22:  # ConfigureNotify: type, serial, send_event, display, event, window, x, y, w, h
            fields = ctypes.cast(ctypes.byref(event, 6 * 8), ctypes.POINTER(ctypes.c_int))
            size[:] = [fields[2], fields[3]]
            out(kind='configure', synthetic=bool(raw[4]), x=fields[0], y=fields[1], width=fields[2], height=fields[3])
            paint()
        elif kind == 12:  # Expose
            paint()
        elif kind == 4:  # ButtonPress: ..., window, root, subwindow, time, x, y, x_root, y_root
            fields = ctypes.cast(ctypes.byref(event, 8 * 8), ctypes.POINTER(ctypes.c_int))
            root_ret, child, rx, ry, wx, wy, mask = (ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_int(), ctypes.c_int(),
                                                    ctypes.c_int(), ctypes.c_int(), ctypes.c_uint())
            x.XQueryPointer(dpy, win, ctypes.byref(root_ret), ctypes.byref(child), ctypes.byref(rx), ctypes.byref(ry),
                            ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask))
            out(kind='button', x=fields[0], y=fields[1], x_root=fields[2], y_root=fields[3],
                query_root=[rx.value, ry.value], query_window=[wx.value, wy.value],
                state=atoms(x, dpy, win, b'_NET_WM_STATE'))
            if args.popup:  # a menu-like override-redirect window at the click, in root coordinates
                attributes = (ctypes.c_long * 16)()
                attributes[11] = 1  # XSetWindowAttributes.override_redirect (byte 88)
                x.XCreateWindow.restype = ctypes.c_ulong
                x.XCreateWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                            ctypes.c_uint, ctypes.c_uint, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p,
                                            ctypes.c_ulong, ctypes.c_void_p]
                popup = x.XCreateWindow(dpy, root, fields[2], fields[3], 120, 80, 0, 0, 1, None, 1 << 9, attributes)
                x.XSelectInput(dpy, popup, 1 << 15)
                x.XMapWindow(dpy, popup)
                popup_gc = x.XCreateGC(dpy, popup, 0, None)
                x.XSetForeground(dpy, popup_gc, 0xffa000)
                for _ in range(20):  # paint once it is viewable
                    x.XFillRectangle(dpy, popup, popup_gc, 0, 0, 120, 80)
                    x.XFlush(dpy)
                    time.sleep(0.05)
                out(kind='popup', at=[fields[2], fields[3]], size=[120, 80])
        del longs


def spy(x, dpy, args):
    """Root SubstructureNotify listener: EWMH client messages and toplevel configures."""
    root = x.XRootWindow(dpy, x.XDefaultScreen(dpy))
    x.XSelectInput(dpy, root, 1 << 19)
    x.XFlush(dpy)

    def name(atom):
        if not atom:
            return None
        pointer = x.XGetAtomName(dpy, atom)
        text = ctypes.cast(pointer, ctypes.c_char_p).value.decode(errors='replace') if pointer else str(atom)
        if pointer:
            x.XFree(pointer)
        return text
    event = XEvent()
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        if not x.XPending(dpy):
            time.sleep(0.02)
            continue
        x.XNextEvent(dpy, ctypes.byref(event))
        longs = ctypes.cast(ctypes.byref(event), ctypes.POINTER(ctypes.c_long))
        if event.type == 33:  # ClientMessage: type, serial, send_event, display, window, message_type, format, data
            fmt = ctypes.cast(ctypes.byref(event, 48), ctypes.POINTER(ctypes.c_int))[0]
            data = [longs[7 + i] for i in range(5)]
            out(kind='client-message', window=longs[4], type=name(longs[5]), format=fmt, data=data,
                atoms=[name(v) if 0 < v < 1 << 29 else v for v in data[:3]])
        elif event.type == 22:
            fields = ctypes.cast(ctypes.byref(event, 6 * 8), ctypes.POINTER(ctypes.c_int))
            out(kind='root-configure', window=longs[5], x=fields[0], y=fields[1], width=fields[2], height=fields[3])
        elif event.type in (19, 20):
            out(kind='map' if event.type == 19 else 'map-request', window=longs[5 if event.type == 19 else 5])


def sdl(args):
    s = library('/nix/store/*-sdl2-compat-2.*/lib/libSDL2-2.0.so.0')
    I = ctypes.c_int
    s.SDL_CreateWindow.restype = ctypes.c_void_p
    s.SDL_CreateWindow.argtypes = [ctypes.c_char_p, I, I, I, I, ctypes.c_uint32]
    s.SDL_GetWindowPosition.argtypes = [ctypes.c_void_p, ctypes.POINTER(I), ctypes.POINTER(I)]
    s.SDL_GetWindowSize.argtypes = [ctypes.c_void_p, ctypes.POINTER(I), ctypes.POINTER(I)]
    s.SDL_GetGlobalMouseState.argtypes = [ctypes.POINTER(I), ctypes.POINTER(I)]
    s.SDL_GetGlobalMouseState.restype = ctypes.c_uint32
    s.SDL_GetError.restype = ctypes.c_char_p
    if s.SDL_Init(0x20):
        raise SystemExit('SDL_Init: ' + s.SDL_GetError().decode())
    # Like MonoGame: an OpenGL window from creation (a later GL/surface switch
    # makes SDL3 recreate the X window), SHOWN | RESIZABLE | FULLSCREEN_DESKTOP.
    flags = 0x2 | 0x4 | 0x20 | (0x1001 if args.fullscreen else 0)
    win = s.SDL_CreateWindow(b'sdl-probe', 0x2FFF0000, 0x2FFF0000, 640, 400, flags)
    if not win:
        raise SystemExit('SDL_CreateWindow: ' + s.SDL_GetError().decode())

    def geometry():
        px, py, w, h = I(), I(), I(), I()
        s.SDL_GetWindowPosition(win, ctypes.byref(px), ctypes.byref(py))
        s.SDL_GetWindowSize(win, ctypes.byref(w), ctypes.byref(h))
        return [px.value, py.value], [w.value, h.value]

    s.SDL_GL_CreateContext.restype = ctypes.c_void_p
    s.SDL_GL_CreateContext.argtypes = [ctypes.c_void_p]
    s.SDL_GL_GetProcAddress.restype = ctypes.c_void_p
    s.SDL_GL_GetProcAddress.argtypes = [ctypes.c_char_p]
    s.SDL_GL_SwapWindow.argtypes = [ctypes.c_void_p]
    s.SDL_GL_GetDrawableSize.argtypes = [ctypes.c_void_p, ctypes.POINTER(I), ctypes.POINTER(I)]
    if not s.SDL_GL_CreateContext(win):
        raise SystemExit('SDL_GL_CreateContext: ' + s.SDL_GetError().decode())

    def gl(name, *argtypes):
        return ctypes.CFUNCTYPE(None, *argtypes)(s.SDL_GL_GetProcAddress(name))
    F = ctypes.c_float
    clear_color, clear, scissor = gl(b'glClearColor', F, F, F, F), gl(b'glClear', ctypes.c_uint), gl(b'glScissor', I, I, I, I)
    enable, disable, viewport = gl(b'glEnable', ctypes.c_uint), gl(b'glDisable', ctypes.c_uint), gl(b'glViewport', I, I, I, I)

    def paint():
        # GL origin is bottom-left. Corner squares: red TL, green TR, blue BL, white BR.
        w, h = I(), I()
        s.SDL_GL_GetDrawableSize(win, ctypes.byref(w), ctypes.byref(h))
        w, h = w.value, h.value
        viewport(0, 0, w, h)
        disable(0x0C11)
        clear_color(0.13, 0.19, 0.25, 1)
        clear(0x4000)
        enable(0x0C11)  # GL_SCISSOR_TEST
        for (r, g, b), rx, ry in (((1, 0, 0), 0, h - 32), ((0, 1, 0), w - 32, h - 32), ((0, 0, 1), 0, 0),
                                  ((1, 1, 1), w - 32, 0)):
            scissor(rx, ry, 32, 32)
            clear_color(r, g, b, 1)
            clear(0x4000)
        disable(0x0C11)
        s.SDL_GL_SwapWindow(win)

    position, size = geometry()
    out(kind='sdl-window', position=position, size=size, driver=os.environ.get('SDL_VIDEODRIVER'))
    paint()
    event = (ctypes.c_uint8 * 64)()
    deadline = time.monotonic() + args.seconds
    repaint = 0
    while time.monotonic() < deadline:
        if not s.SDL_PollEvent(ctypes.byref(event)):
            if time.monotonic() > repaint:  # EGL surfaces resize only after a swap
                paint()
                repaint = time.monotonic() + 1
            time.sleep(0.02)
            continue
        kind = ctypes.cast(event, ctypes.POINTER(ctypes.c_uint32))[0]
        ints = ctypes.cast(event, ctypes.POINTER(ctypes.c_int32))
        if kind == 0x100:  # SDL_QUIT (native close)
            out(kind='sdl-quit')
            return
        if kind == 0x200:  # window event: event byte at 12, data1/data2 at 16/20
            sub = event[12]
            if sub in (4, 5, 6):  # MOVED, RESIZED, SIZE_CHANGED
                position, size = geometry()
                out(kind='sdl-windowevent', event={4: 'moved', 5: 'resized', 6: 'size_changed'}[sub],
                    data=[ints[4], ints[5]], position=position, size=size)
                paint()
            elif sub == 3:  # EXPOSED
                paint()
        elif kind == 0x401:  # MOUSEBUTTONDOWN: x/y at 20/24
            gx, gy = I(), I()
            buttons = s.SDL_GetGlobalMouseState(ctypes.byref(gx), ctypes.byref(gy))
            position, size = geometry()
            out(kind='sdl-button', event=[ints[5], ints[6]], global_mouse=[gx.value, gy.value], global_buttons=buttons,
                position=position, size=size, monogame=[gx.value - position[0], gy.value - position[1]])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('info', 'window', 'sdl', 'spy'))
    parser.add_argument('--display', required=True, help='guest X display, e.g. :0')
    parser.add_argument('--fullscreen', action='store_true')
    parser.add_argument('--popup', action='store_true', help='window mode: map an override-redirect popup at each click')
    parser.add_argument('--class', dest='window_class', default='brook-x11-probe')
    parser.add_argument('--size', default='400x300')
    parser.add_argument('--position', type=int, nargs=2, default=(120, 160))
    parser.add_argument('--seconds', type=float, default=60)
    args = parser.parse_args()
    if not args.display.startswith(':') or not args.display[1:].isdigit():
        parser.error('--display must be a local :N display')
    os.environ['DISPLAY'] = args.display
    os.environ.pop('WAYLAND_DISPLAY', None)
    os.environ['SDL_VIDEODRIVER'] = 'x11'
    if args.mode == 'sdl':
        # Brook's default Xwayland has no GLX; SDL's EGL path renders (llvmpipe).
        os.environ.setdefault('SDL_VIDEO_FORCE_EGL', '1')
        os.environ.setdefault('SDL_VIDEO_X11_FORCE_EGL', '1')
        os.environ.setdefault('LIBGL_ALWAYS_SOFTWARE', '1')
        return sdl(args)
    x = xlib()
    dpy = x.XOpenDisplay(args.display.encode())
    if not dpy:
        raise SystemExit(f'cannot open {args.display}')
    info(x, dpy)
    if args.mode == 'window':
        window(x, dpy, args)
    if args.mode == 'spy':
        spy(x, dpy, args)


if __name__ == '__main__':
    sys.exit(main())

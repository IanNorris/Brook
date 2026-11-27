/* Rootless Xwayland input/cursor probe (runs inside Brook, never on the host).
 *
 * Opens three top-level X windows on DISPLAY: "xwl-probe-a" and
 * "xwl-probe-b" (overlapping in X coordinates) and "xwl-probe-dialog",
 * which is created before the others are mapped but mapped only after
 * --dialog-delay seconds, with WM_TRANSIENT_FOR on A. That reproduces a
 * toolkit (Steam/CEF) that creates its windows up front and shows them
 * later. No window defines a cursor, so each inherits the root cursor.
 *
 * Window A is a depth-24 window whose pixels are written by XPutImage with
 * the unused top byte set to 0x40 (as a GL client's alpha would be): the
 * compositor must show it opaque, colour 0xc04040.
 *
 * Every ButtonPress, KeyPress, EnterNotify and FocusIn is printed as one
 * line "EV <window-name> <event> <detail> <x> <y>", flushed, so a host
 * harness can check which window received which input. Exits after
 * --seconds.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct probe { const char *name; int x, y; unsigned w, h; unsigned long pixel; Window id; };

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    double seconds = 120, dialog_delay = 8;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--dialog-delay")) dialog_delay = atof(argv[i + 1]);
    }
    Display *d = NULL;
    for (int tries = 0; !d && tries < 60; ++tries) {  /* the owned Xwayland may still be starting */
        d = XOpenDisplay(NULL);
        if (!d) { struct timespec pause = {0, 500000000}; nanosleep(&pause, NULL); }
    }
    if (!d) { fprintf(stderr, "cannot open display\n"); return 1; }
    int screen = DefaultScreen(d);
    Window root = RootWindow(d, screen);
    struct probe w[3] = {
        {"xwl-probe-a", 120, 120, 520, 360, 0xc04040, 0},
        {"xwl-probe-dialog", 260, 200, 320, 200, 0x40a040, 0},
        {"xwl-probe-b", 420, 300, 420, 300, 0x4060c0, 0},
    };
    Atom protocols = XInternAtom(d, "WM_PROTOCOLS", False);
    Atom delete_window = XInternAtom(d, "WM_DELETE_WINDOW", False);
    for (int i = 0; i < 3; ++i) {
        XSetWindowAttributes a = {0};
        a.background_pixel = w[i].pixel;
        a.event_mask = ButtonPressMask | KeyPressMask | EnterWindowMask | FocusChangeMask |
                       StructureNotifyMask | ExposureMask;
        w[i].id = XCreateWindow(d, root, w[i].x, w[i].y, w[i].w, w[i].h, 0, CopyFromParent,
                                InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &a);
        XStoreName(d, w[i].id, w[i].name);
        XClassHint klass = {(char *)w[i].name, "XwlProbe"};
        XSetClassHint(d, w[i].id, &klass);
        XWMHints hints = {.flags = InputHint, .input = True};
        XSetWMHints(d, w[i].id, &hints);
        XSizeHints size = {.flags = PPosition | PSize, .x = w[i].x, .y = w[i].y,
                           .width = (int)w[i].w, .height = (int)w[i].h};
        XSetWMNormalHints(d, w[i].id, &size);
        XSetWMProtocols(d, w[i].id, &delete_window, 1);
    }
    XSetTransientForHint(d, w[1].id, w[0].id);
    XMapWindow(d, w[0].id);
    XMapWindow(d, w[2].id);
    XFlush(d);
    for (int i = 0; i < 3; ++i) printf("WIN %s 0x%lx\n", w[i].name, w[i].id);
    fflush(stdout);
    const double start = now_s();
    int dialog_mapped = 0;
    while (now_s() - start < seconds) {
        if (!dialog_mapped && now_s() - start >= dialog_delay) {
            XMapWindow(d, w[1].id); XFlush(d); dialog_mapped = 1;
            printf("MAPPED xwl-probe-dialog %.1f\n", now_s() - start); fflush(stdout);
        }
        if (!XPending(d)) {
            struct pollfd p = {ConnectionNumber(d), POLLIN, 0};
            poll(&p, 1, 100);
            continue;
        }
        XEvent e; XNextEvent(d, &e);
        const char *name = "?";
        for (int i = 0; i < 3; ++i) if (e.xany.window == w[i].id) name = w[i].name;
        switch (e.type) {
        case ButtonPress:
            printf("EV %s ButtonPress %u %d %d\n", name, e.xbutton.button, e.xbutton.x, e.xbutton.y); break;
        case KeyPress: {
            char text[8] = {0}; KeySym sym;
            XLookupString(&e.xkey, text, sizeof(text) - 1, &sym, NULL);
            printf("EV %s KeyPress %s %d %d\n", name, XKeysymToString(sym) ? XKeysymToString(sym) : "?",
                   e.xkey.x, e.xkey.y);
            break;
        }
        case EnterNotify: {
            printf("EV %s EnterNotify %d %d %d\n", name, e.xcrossing.detail, e.xcrossing.x, e.xcrossing.y);
            /* The cursor X shows over this window (inherited from the root:
             * no probe window defines one). The server default is the core
             * "X" glyph; a theme left_ptr is typically 24x24 or larger. */
            XFixesCursorImage *cursor = XFixesGetCursorImage(d);
            if (cursor) {
                unsigned long opaque = 0;
                for (unsigned i = 0; i < (unsigned)cursor->width * cursor->height; ++i)
                    if ((cursor->pixels[i] >> 24) & 0xff) ++opaque;
                printf("CURSOR %s %ux%u hot=%u,%u opaque=%lu serial=%lu\n", name, cursor->width, cursor->height,
                       cursor->xhot, cursor->yhot, opaque, cursor->cursor_serial);
                XFree(cursor);
            }
            break;
        }
        case FocusIn:
            printf("EV %s FocusIn %d 0 0\n", name, e.xfocus.detail); break;
        case Expose:
            if (e.xexpose.window == w[0].id && !e.xexpose.count) {
                XWindowAttributes attributes;
                XGetWindowAttributes(d, w[0].id, &attributes);
                XImage *image = XCreateImage(d, attributes.visual, (unsigned)attributes.depth, ZPixmap, 0, NULL,
                                             (unsigned)attributes.width, (unsigned)attributes.height, 32, 0);
                if (image && image->bits_per_pixel == 32) {
                    image->data = malloc((size_t)image->bytes_per_line * (size_t)image->height);
                    for (int y = 0; image->data && y < image->height; ++y)
                        for (int x = 0; x < image->width; ++x)
                            ((unsigned *)(image->data + (size_t)y * image->bytes_per_line))[x] = 0x40c04040u;
                    if (image->data) {
                        XPutImage(d, w[0].id, DefaultGC(d, screen), image, 0, 0, 0, 0,
                                  (unsigned)image->width, (unsigned)image->height);
                        printf("PUT xwl-probe-a depth=%d alpha=0x40\n", attributes.depth);
                    }
                }
                if (image) XDestroyImage(image);
            }
            break;
        case ConfigureNotify:
            printf("CFG %s %d %d %d %d\n", name, e.xconfigure.x, e.xconfigure.y,
                   e.xconfigure.width, e.xconfigure.height); break;
        case ClientMessage:
            if ((Atom)e.xclient.message_type == protocols && (Atom)e.xclient.data.l[0] == delete_window) {
                printf("CLOSE %s\n", name); fflush(stdout); return 0;
            }
            break;
        default: break;
        }
        fflush(stdout);
    }
    XCloseDisplay(d);
    return 0;
}

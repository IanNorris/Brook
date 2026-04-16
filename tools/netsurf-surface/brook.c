/*
 * Brook OS framebuffer surface for libnsfb
 *
 * Renders to /dev/fb0 using mmap, reads PS/2 keyboard and mouse.
 * Based on Brook's quakegeneric_brook.c platform layer.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"

/* Brook-specific state */
typedef struct {
    int fb_fd;
    int kb_fd;
    int mouse_fd;
    uint32_t *fb_ptr;       /* mmap'd framebuffer */
    int fb_width;           /* physical framebuffer width */
    int fb_height;          /* physical framebuffer height */
    int mouse_x;            /* absolute cursor position */
    int mouse_y;
} brook_priv_t;

/* PS/2 set-1 scancode → NSFB key code */
static enum nsfb_key_code_e scancode_to_nsfb(unsigned char sc)
{
    switch (sc) {
    case 0x01: return NSFB_KEY_ESCAPE;
    case 0x1C: return NSFB_KEY_RETURN;
    case 0x39: return NSFB_KEY_SPACE;
    case 0x0E: return NSFB_KEY_BACKSPACE;
    case 0x0F: return NSFB_KEY_TAB;
    case 0x48: return NSFB_KEY_UP;
    case 0x50: return NSFB_KEY_DOWN;
    case 0x4B: return NSFB_KEY_LEFT;
    case 0x4D: return NSFB_KEY_RIGHT;
    case 0x1D: return NSFB_KEY_LCTRL;
    case 0x38: return NSFB_KEY_LALT;
    case 0x2A: return NSFB_KEY_LSHIFT;
    case 0x36: return NSFB_KEY_RSHIFT;
    case 0x53: return NSFB_KEY_DELETE;
    case 0x47: return NSFB_KEY_HOME;
    case 0x4F: return NSFB_KEY_END;
    case 0x49: return NSFB_KEY_PAGEUP;
    case 0x51: return NSFB_KEY_PAGEDOWN;
    case 0x3B: return NSFB_KEY_F1;
    case 0x3C: return NSFB_KEY_F2;
    case 0x3D: return NSFB_KEY_F3;
    case 0x3E: return NSFB_KEY_F4;
    case 0x3F: return NSFB_KEY_F5;
    case 0x40: return NSFB_KEY_F6;
    case 0x41: return NSFB_KEY_F7;
    case 0x42: return NSFB_KEY_F8;
    case 0x43: return NSFB_KEY_F9;
    case 0x44: return NSFB_KEY_F10;
    case 0x57: return NSFB_KEY_F11;
    case 0x58: return NSFB_KEY_F12;

    /* Number row */
    case 0x02: return NSFB_KEY_1;
    case 0x03: return NSFB_KEY_2;
    case 0x04: return NSFB_KEY_3;
    case 0x05: return NSFB_KEY_4;
    case 0x06: return NSFB_KEY_5;
    case 0x07: return NSFB_KEY_6;
    case 0x08: return NSFB_KEY_7;
    case 0x09: return NSFB_KEY_8;
    case 0x0A: return NSFB_KEY_9;
    case 0x0B: return NSFB_KEY_0;
    case 0x0C: return NSFB_KEY_MINUS;
    case 0x0D: return NSFB_KEY_EQUALS;

    /* Letters */
    case 0x10: return NSFB_KEY_q;
    case 0x11: return NSFB_KEY_w;
    case 0x12: return NSFB_KEY_e;
    case 0x13: return NSFB_KEY_r;
    case 0x14: return NSFB_KEY_t;
    case 0x15: return NSFB_KEY_y;
    case 0x16: return NSFB_KEY_u;
    case 0x17: return NSFB_KEY_i;
    case 0x18: return NSFB_KEY_o;
    case 0x19: return NSFB_KEY_p;
    case 0x1A: return NSFB_KEY_LEFTBRACKET;
    case 0x1B: return NSFB_KEY_RIGHTBRACKET;
    case 0x1E: return NSFB_KEY_a;
    case 0x1F: return NSFB_KEY_s;
    case 0x20: return NSFB_KEY_d;
    case 0x21: return NSFB_KEY_f;
    case 0x22: return NSFB_KEY_g;
    case 0x23: return NSFB_KEY_h;
    case 0x24: return NSFB_KEY_j;
    case 0x25: return NSFB_KEY_k;
    case 0x26: return NSFB_KEY_l;
    case 0x27: return NSFB_KEY_SEMICOLON;
    case 0x28: return NSFB_KEY_QUOTE;
    case 0x29: return NSFB_KEY_BACKQUOTE;
    case 0x2B: return NSFB_KEY_BACKSLASH;
    case 0x2C: return NSFB_KEY_z;
    case 0x2D: return NSFB_KEY_x;
    case 0x2E: return NSFB_KEY_c;
    case 0x2F: return NSFB_KEY_v;
    case 0x30: return NSFB_KEY_b;
    case 0x31: return NSFB_KEY_n;
    case 0x32: return NSFB_KEY_m;
    case 0x33: return NSFB_KEY_COMMA;
    case 0x34: return NSFB_KEY_PERIOD;
    case 0x35: return NSFB_KEY_SLASH;

    default: return NSFB_KEY_UNKNOWN;
    }
}

static int brook_defaults(nsfb_t *nsfb)
{
    nsfb->width = 800;
    nsfb->height = 600;
    nsfb->format = NSFB_FMT_XRGB8888;

    select_plotters(nsfb);
    return 0;
}

static int brook_initialise(nsfb_t *nsfb)
{
    brook_priv_t *priv = calloc(1, sizeof(brook_priv_t));
    if (!priv) return -1;

    priv->fb_fd = -1;
    priv->kb_fd = -1;
    priv->mouse_fd = -1;

    /* Open framebuffer */
    priv->fb_fd = open("/dev/fb0", O_RDWR);
    if (priv->fb_fd >= 0) {
        struct fb_var_screeninfo vinfo;
        if (ioctl(priv->fb_fd, 0x4600 /*FBIOGET_VSCREENINFO*/, &vinfo) == 0) {
            priv->fb_width = vinfo.xres;
            priv->fb_height = vinfo.yres;
            unsigned long size = priv->fb_width * priv->fb_height * 4;
            priv->fb_ptr = mmap(0, size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, priv->fb_fd, 0);
            if ((long)priv->fb_ptr == -1)
                priv->fb_ptr = NULL;
        }
        fprintf(stderr, "brook-nsfb: fb %dx%d, window %dx%d\n",
                priv->fb_width, priv->fb_height, nsfb->width, nsfb->height);
    }

    /* Open keyboard (non-blocking) */
    priv->kb_fd = open("/dev/keyboard", O_RDONLY);
    if (priv->kb_fd >= 0)
        ioctl(priv->kb_fd, 1, (void *)1);

    /* Open mouse (non-blocking) */
    priv->mouse_fd = open("/dev/mouse", O_RDONLY);
    if (priv->mouse_fd >= 0)
        ioctl(priv->mouse_fd, 1, (void *)1);

    /* Center mouse cursor */
    priv->mouse_x = nsfb->width / 2;
    priv->mouse_y = nsfb->height / 2;

    nsfb->surface_priv = priv;

    /* Allocate internal render buffer (NetSurf draws into this) */
    size_t bufsize = (nsfb->width * nsfb->height * nsfb->bpp) / 8;
    nsfb->ptr = calloc(1, bufsize);
    if (!nsfb->ptr) {
        free(priv);
        return -1;
    }
    nsfb->linelen = (nsfb->width * nsfb->bpp) / 8;

    return 0;
}

static int brook_finalise(nsfb_t *nsfb)
{
    brook_priv_t *priv = nsfb->surface_priv;
    if (priv) {
        if (priv->fb_fd >= 0) close(priv->fb_fd);
        if (priv->kb_fd >= 0) close(priv->kb_fd);
        if (priv->mouse_fd >= 0) close(priv->mouse_fd);
        free(priv);
    }
    free(nsfb->ptr);
    nsfb->ptr = NULL;
    return 0;
}

static int brook_set_geometry(nsfb_t *nsfb, int width, int height,
                              enum nsfb_format_e format)
{
    if (width > 0) nsfb->width = width;
    if (height > 0) nsfb->height = height;
    if (format != NSFB_FMT_ANY) nsfb->format = format;

    select_plotters(nsfb);

    size_t bufsize = (nsfb->width * nsfb->height * nsfb->bpp) / 8;
    uint8_t *newbuf = realloc(nsfb->ptr, bufsize);
    if (!newbuf) return -1;
    nsfb->ptr = newbuf;
    nsfb->linelen = (nsfb->width * nsfb->bpp) / 8;

    return 0;
}

static bool brook_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    brook_priv_t *priv = nsfb->surface_priv;
    if (!priv) return false;

    (void)timeout;

    /* Poll keyboard */
    if (priv->kb_fd >= 0) {
        unsigned char sc;
        if (read(priv->kb_fd, &sc, 1) > 0) {
            int released = (sc & 0x80);
            enum nsfb_key_code_e key = scancode_to_nsfb(sc & 0x7F);
            if (key != NSFB_KEY_UNKNOWN) {
                event->type = released ? NSFB_EVENT_KEY_UP : NSFB_EVENT_KEY_DOWN;
                event->value.keycode = key;
                return true;
            }
        }
    }

    /* Poll mouse — 3-byte PS/2 packets */
    if (priv->mouse_fd >= 0) {
        unsigned char pkt[3];
        if (read(priv->mouse_fd, pkt, 3) == 3) {
            int flags = pkt[0];
            int dx = pkt[1];
            int dy = pkt[2];
            if (flags & 0x10) dx |= ~0xFF;
            if (flags & 0x20) dy |= ~0xFF;

            priv->mouse_x += dx;
            priv->mouse_y -= dy; /* PS/2 Y is inverted */

            /* Clamp to window */
            if (priv->mouse_x < 0) priv->mouse_x = 0;
            if (priv->mouse_y < 0) priv->mouse_y = 0;
            if (priv->mouse_x >= nsfb->width) priv->mouse_x = nsfb->width - 1;
            if (priv->mouse_y >= nsfb->height) priv->mouse_y = nsfb->height - 1;

            /* Report absolute position + buttons */
            event->type = NSFB_EVENT_MOVE_ABSOLUTE;
            event->value.vector.x = priv->mouse_x;
            event->value.vector.y = priv->mouse_y;
            event->value.vector.z = 0;
            /* Button state in high bits: bit 0 = LMB */
            if (flags & 0x01) event->value.vector.z |= 1;
            if (flags & 0x02) event->value.vector.z |= 4; /* RMB */
            return true;
        }
    }

    return false;
}

/* Blit the internal buffer to the physical framebuffer */
static int brook_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    brook_priv_t *priv = nsfb->surface_priv;
    if (!priv || !priv->fb_ptr || !nsfb->ptr) return 0;

    /* Compute scale and offset to center window in physical fb */
    int sw = nsfb->width;
    int sh = nsfb->height;
    int dw = priv->fb_width;
    int dh = priv->fb_height;

    int ox = (dw - sw) / 2;
    int oy = (dh - sh) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    /* Only copy the dirty rectangle */
    int y0 = box->y0;
    int y1 = box->y1;
    int x0 = box->x0;
    int x1 = box->x1;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > sh) y1 = sh;
    if (x1 > sw) x1 = sw;

    uint32_t *src = (uint32_t *)nsfb->ptr;
    for (int y = y0; y < y1 && (y + oy) < dh; y++) {
        uint32_t *srow = src + y * sw;
        uint32_t *drow = priv->fb_ptr + (y + oy) * dw + ox;
        int copyW = x1 - x0;
        if (x0 + ox + copyW > dw) copyW = dw - x0 - ox;
        if (copyW > 0)
            memcpy(drow + x0, srow + x0, copyW * 4);
    }

    /* Signal compositor */
    if (priv->fb_fd >= 0)
        write(priv->fb_fd, "", 1);

    return 0;
}

static int brook_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)nsfb;
    (void)box;
    return 0;
}

const nsfb_surface_rtns_t brook_rtns = {
    .defaults = brook_defaults,
    .initialise = brook_initialise,
    .finalise = brook_finalise,
    .geometry = brook_set_geometry,
    .input = brook_input,
    .claim = brook_claim,
    .update = brook_update,
};

NSFB_SURFACE_DEF(brook, NSFB_SURFACE_LINUX, &brook_rtns)

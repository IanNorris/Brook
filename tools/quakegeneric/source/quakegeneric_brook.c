// quakegeneric_brook.c — Brook OS platform layer for Quake
//
// Provides framebuffer output (palette→RGBA), PS/2 keyboard input,
// PS/2 mouse input, and timing.

#include "quakegeneric.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <time.h>
#include <sched.h>

static int fb_fd = -1;
static int *fb_pixels = NULL;
static int kb_fd = -1;
static int mouse_fd = -1;

static unsigned int screen_w = 0;
static unsigned int screen_h = 0;

// Current 256-entry RGBA palette (built from QG_SetPalette)
static unsigned int palette_rgba[256];

// Keyboard queue
#define KEYQUEUE_SIZE 64
static struct { int down; int key; } key_queue[KEYQUEUE_SIZE];
static unsigned int kq_write = 0;
static unsigned int kq_read = 0;

// Mouse delta accumulator
static int mouse_dx = 0;
static int mouse_dy = 0;

// Map PS/2 set 1 scancodes to Quake keys
static int scancode_to_quake(unsigned char sc)
{
    switch (sc)
    {
    case 0x01: return K_ESCAPE;
    case 0x1C: return K_ENTER;
    case 0x39: return K_SPACE;
    case 0x0E: return K_BACKSPACE;
    case 0x0F: return K_TAB;

    // Arrow keys
    case 0x48: return K_UPARROW;
    case 0x50: return K_DOWNARROW;
    case 0x4B: return K_LEFTARROW;
    case 0x4D: return K_RIGHTARROW;

    // Modifiers
    case 0x1D: return K_CTRL;
    case 0x38: return K_ALT;
    case 0x2A: case 0x36: return K_SHIFT;

    // Function keys
    case 0x3B: return K_F1;
    case 0x3C: return K_F2;
    case 0x3D: return K_F3;
    case 0x3E: return K_F4;
    case 0x3F: return K_F5;
    case 0x40: return K_F6;
    case 0x41: return K_F7;
    case 0x42: return K_F8;
    case 0x43: return K_F9;
    case 0x44: return K_F10;
    case 0x57: return K_F11;
    case 0x58: return K_F12;

    // Number row
    case 0x02: return '1';
    case 0x03: return '2';
    case 0x04: return '3';
    case 0x05: return '4';
    case 0x06: return '5';
    case 0x07: return '6';
    case 0x08: return '7';
    case 0x09: return '8';
    case 0x0A: return '9';
    case 0x0B: return '0';
    case 0x0C: return '-';
    case 0x0D: return '=';

    // Letters (QWERTY)
    case 0x10: return 'q';
    case 0x11: return 'w';
    case 0x12: return 'e';
    case 0x13: return 'r';
    case 0x14: return 't';
    case 0x15: return 'y';
    case 0x16: return 'u';
    case 0x17: return 'i';
    case 0x18: return 'o';
    case 0x19: return 'p';
    case 0x1E: return 'a';
    case 0x1F: return 's';
    case 0x20: return 'd';
    case 0x21: return 'f';
    case 0x22: return 'g';
    case 0x23: return 'h';
    case 0x24: return 'j';
    case 0x25: return 'k';
    case 0x26: return 'l';
    case 0x2C: return 'z';
    case 0x2D: return 'x';
    case 0x2E: return 'c';
    case 0x2F: return 'v';
    case 0x30: return 'b';
    case 0x31: return 'n';
    case 0x32: return 'm';

    // Punctuation
    case 0x27: return ';';
    case 0x28: return '\'';
    case 0x29: return '`';
    case 0x2B: return '\\';
    case 0x33: return ',';
    case 0x34: return '.';
    case 0x35: return '/';
    case 0x1A: return '[';
    case 0x1B: return ']';

    default: return 0;
    }
}

static void enqueue_key(int down, int key)
{
    unsigned int next = (kq_write + 1) % KEYQUEUE_SIZE;
    if (next == kq_read) return; // queue full
    key_queue[kq_write].down = down;
    key_queue[kq_write].key = key;
    kq_write = next;
}

static void poll_keyboard(void)
{
    if (kb_fd < 0) return;

    unsigned char sc;
    while (read(kb_fd, &sc, 1) > 0)
    {
        int released = (sc & 0x80);
        int key = scancode_to_quake(sc & 0x7F);
        if (key == 0) continue;
        enqueue_key(!released, key);
    }
}

// PS/2 mouse: 3-byte packets [flags, dx, dy]
static void poll_mouse(void)
{
    if (mouse_fd < 0) return;

    unsigned char pkt[3];
    while (read(mouse_fd, pkt, 3) == 3)
    {
        int flags = pkt[0];

        // Reconstruct signed dx/dy from 8-bit + sign bits
        int dx = pkt[1];
        int dy = pkt[2];
        if (flags & 0x10) dx |= ~0xFF; // sign-extend
        if (flags & 0x20) dy |= ~0xFF;

        mouse_dx += dx;
        mouse_dy -= dy; // PS/2 Y is inverted vs screen

        // Enqueue mouse button events
        static int prev_lmb = 0, prev_rmb = 0;
        int lmb = (flags & 0x01);
        int rmb = (flags & 0x02);
        if (lmb != prev_lmb) { enqueue_key(lmb, K_MOUSE1); prev_lmb = lmb; }
        if (rmb != prev_rmb) { enqueue_key(rmb, K_MOUSE2); prev_rmb = rmb; }
    }
}

void QG_Init(void)
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd >= 0)
    {
        struct fb_var_screeninfo vinfo;
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0)
        {
            screen_w = vinfo.xres;
            screen_h = vinfo.yres;

            unsigned long screensize = screen_w * screen_h * (vinfo.bits_per_pixel / 8);
            fb_pixels = (int *)mmap(0, screensize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fb_fd, 0);
            if ((long)fb_pixels == -1)
                fb_pixels = NULL;
        }
        fprintf(stderr, "Brook Quake: fb %ux%u, render %ux%u\n",
                screen_w, screen_h, QUAKEGENERIC_RES_X, QUAKEGENERIC_RES_Y);
    }

    kb_fd = open("/dev/keyboard", O_RDONLY);
    if (kb_fd >= 0)
        ioctl(kb_fd, 1, (void *)1); // non-blocking mode

    mouse_fd = open("/dev/mouse", O_RDONLY);
    if (mouse_fd >= 0)
        ioctl(mouse_fd, 1, (void *)1); // non-blocking mode
}

void QG_Quit(void)
{
}

void QG_SetPalette(unsigned char palette[768])
{
    for (int i = 0; i < 256; i++)
    {
        unsigned int r = palette[i * 3 + 0];
        unsigned int g = palette[i * 3 + 1];
        unsigned int b = palette[i * 3 + 2];
        palette_rgba[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }
}

void QG_DrawFrame(void *pixels)
{
    if (!fb_pixels) return;

    const unsigned char *src = (const unsigned char *)pixels;

    // Auto-fit: largest integer scale that fits in the VFB
    int scaleX = (int)screen_w / QUAKEGENERIC_RES_X;
    int scaleY = (int)screen_h / QUAKEGENERIC_RES_Y;
    int scale = scaleX < scaleY ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int draw_w = QUAKEGENERIC_RES_X * scale;
    int draw_h = QUAKEGENERIC_RES_Y * scale;

    // Center in the VFB
    int ox = ((int)screen_w - draw_w) / 2;
    int oy = ((int)screen_h - draw_h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    if (scale == 1)
    {
        // Fast path: no scaling, convert palette inline
        for (int y = 0; y < QUAKEGENERIC_RES_Y && (y + oy) < (int)screen_h; y++)
        {
            const unsigned char *row = src + y * QUAKEGENERIC_RES_X;
            int *dst = fb_pixels + (ox + (y + oy) * (int)screen_w);

            for (int x = 0; x < QUAKEGENERIC_RES_X && (x + ox) < (int)screen_w; x++)
                dst[x] = palette_rgba[row[x]];
        }
    }
    else
    {
        for (int y = 0; y < draw_h && (y + oy) < (int)screen_h; y++)
        {
            int sy = y / scale;
            const unsigned char *row = src + sy * QUAKEGENERIC_RES_X;
            int *dst = fb_pixels + (ox + (y + oy) * (int)screen_w);

            for (int x = 0; x < draw_w && (x + ox) < (int)screen_w; x++)
                dst[x] = palette_rgba[row[x / scale]];
        }
    }

    // Signal compositor
    if (fb_fd >= 0)
        write(fb_fd, "", 1);

    poll_keyboard();
    poll_mouse();
}

int QG_GetKey(int *down, int *key)
{
    if (kq_read == kq_write) return 0;

    *down = key_queue[kq_read].down;
    *key  = key_queue[kq_read].key;
    kq_read = (kq_read + 1) % KEYQUEUE_SIZE;
    return 1;
}

void QG_GetMouseMove(int *x, int *y)
{
    *x = mouse_dx;
    *y = mouse_dy;
    mouse_dx = 0;
    mouse_dy = 0;
}

void QG_GetJoyAxes(float *axes)
{
    for (int i = 0; i < QUAKEGENERIC_JOY_MAX_AXES; i++)
        axes[i] = 0.0f;
}

static double get_time_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char *argv[])
{
    QG_Create(argc, argv);

    double oldtime = get_time_seconds() - 0.1;

    for (;;)
    {
        double newtime = get_time_seconds();
        QG_Tick(newtime - oldtime);
        oldtime = newtime;
    }

    return 0;
}

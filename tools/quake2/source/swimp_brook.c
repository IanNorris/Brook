/*
 * swimp_brook.c — Brook OS software renderer window implementation for Quake 2
 *
 * Maps the framebuffer and provides the palette-to-RGBA blit.
 * Replaces swimp_null.c / rw_x11.c / rw_svgalib.c
 */

#include "r_local.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static int fb_fd = -1;
static uint32_t *fb_pixels = NULL;
static unsigned int fb_width = 0;
static unsigned int fb_height = 0;

// 256-entry RGBA palette
static uint32_t palette_rgba[256];

// 8-bit rendered frame (ref_soft renders into this)
static byte *sw_framebuffer = NULL;

int SWimp_Init(void *hInstance, void *wndProc)
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0)
    {
        ri.Con_Printf(PRINT_ALL, "SWimp_Init: can't open /dev/fb0\n");
        return false;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
    {
        ri.Con_Printf(PRINT_ALL, "SWimp_Init: can't get fb info\n");
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    fb_width = vinfo.xres;
    fb_height = vinfo.yres;

    size_t fb_size = fb_width * fb_height * (vinfo.bits_per_pixel / 8);
    fb_pixels = (uint32_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fb_fd, 0);
    if (fb_pixels == MAP_FAILED)
    {
        ri.Con_Printf(PRINT_ALL, "SWimp_Init: mmap failed\n");
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    ri.Con_Printf(PRINT_ALL, "SWimp_Init: framebuffer %ux%u bpp=%u\n",
                  fb_width, fb_height, vinfo.bits_per_pixel);

    return true;
}

rserr_t SWimp_SetMode(int *pwidth, int *pheight, int mode, qboolean fullscreen)
{
    // Use fixed resolution matching framebuffer or a standard Q2 mode
    // Default Q2 software mode is 320x240
    int width = 320;
    int height = 240;

    ri.Con_Printf(PRINT_ALL, "SWimp_SetMode: %dx%d (mode %d)\n", width, height, mode);

    // Allocate the 8-bit software framebuffer
    if (sw_framebuffer)
        free(sw_framebuffer);
    sw_framebuffer = (byte *)malloc(width * height);
    if (!sw_framebuffer)
        return rserr_invalid_mode;

    // Set up vid structure that ref_soft uses
    vid.width = width;
    vid.height = height;
    vid.buffer = sw_framebuffer;
    vid.rowbytes = width;

    *pwidth = width;
    *pheight = height;

    return rserr_ok;
}

void SWimp_SetPalette(const unsigned char *palette)
{
    if (!palette)
        return;

    for (int i = 0; i < 256; i++)
    {
        unsigned char r = palette[i * 4 + 0];
        unsigned char g = palette[i * 4 + 1];
        unsigned char b = palette[i * 4 + 2];
        // BGRA format: blue at offset 0, green at 8, red at 16, alpha at 24
        palette_rgba[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }
}

void SWimp_BeginFrame(float camera_separation)
{
    // Nothing to do — rendering happens into vid.buffer
}

void SWimp_EndFrame(void)
{
    if (!fb_pixels || !sw_framebuffer)
        return;

    // One-time diagnostic
    static int diag_once = 0;
    if (!diag_once) {
        diag_once = 1;
        ri.Con_Printf(PRINT_ALL, "SWimp_EndFrame: vid=%dx%d rowbytes=%d fb=%ux%u scale=%d\n",
                      vid.width, vid.height, vid.rowbytes, fb_width, fb_height,
                      (int)(fb_width / vid.width < fb_height / vid.height ?
                            fb_width / vid.width : fb_height / vid.height));
    }

    // Blit 8-bit paletted frame to 32-bit framebuffer with scaling
    int sw = vid.width;
    int sh = vid.height;
    int scale_x = fb_width / sw;
    int scale_y = fb_height / sh;
    int scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 1) scale = 1;

    int dst_w = sw * scale;
    int dst_h = sh * scale;
    int off_x = (fb_width - dst_w) / 2;
    int off_y = (fb_height - dst_h) / 2;

    for (int y = 0; y < sh; y++)
    {
        const byte *src_row = sw_framebuffer + y * vid.rowbytes;
        for (int sy = 0; sy < scale; sy++)
        {
            uint32_t *dst_row = fb_pixels + (off_y + y * scale + sy) * fb_width + off_x;
            for (int x = 0; x < sw; x++)
            {
                uint32_t pixel = palette_rgba[src_row[x]];
                for (int sx = 0; sx < scale; sx++)
                    dst_row[x * scale + sx] = pixel;
            }
        }
    }

    // Signal compositor that a new frame is ready
    if (fb_fd >= 0)
        write(fb_fd, "", 1);
}

void SWimp_Shutdown(void)
{
    if (sw_framebuffer)
    {
        free(sw_framebuffer);
        sw_framebuffer = NULL;
    }

    if (fb_pixels && fb_pixels != MAP_FAILED)
    {
        munmap(fb_pixels, fb_width * fb_height * 4);
        fb_pixels = NULL;
    }

    if (fb_fd >= 0)
    {
        close(fb_fd);
        fb_fd = -1;
    }
}

void SWimp_AppActivate(qboolean active)
{
}

// Expose framebuffer info for input/video layers
unsigned int Brook_GetFbWidth(void) { return fb_width; }
unsigned int Brook_GetFbHeight(void) { return fb_height; }

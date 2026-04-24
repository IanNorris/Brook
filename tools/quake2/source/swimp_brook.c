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
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

extern qboolean VID_GetModeInfo(int *width, int *height, int mode);

static int fb_fd = -1;
static uint32_t *fb_pixels = NULL;
static unsigned int fb_width = 0;
static unsigned int fb_height = 0;

// 256-entry RGBA palette
static uint32_t palette_rgba[256];

// 8-bit rendered frame (ref_soft renders into this)
static byte *sw_framebuffer = NULL;

// --- Frame diagnostics -----------------------------------------------------

#define FPS_HIST 64
static uint16_t frame_time_us[FPS_HIST];  // frame times in units of 100us (0-6553ms)
static int      fps_head = 0;
static uint64_t fps_last_ns = 0;
static uint64_t fps_window_start_ns = 0;
static int      fps_window_frames = 0;
static int      fps_value = 0;
static float    fps_mean_ms = 0.0f;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Minimal 3x5 digit + small-letter font, packed MSB-first per row.
// Each glyph is 5 rows of 3 bits. Supports: 0-9, space, '.', 'F','P','S','m','s'.
static const uint8_t glyph_digit[10][5] = {
    {0b111,0b101,0b101,0b101,0b111}, // 0
    {0b010,0b110,0b010,0b010,0b111}, // 1
    {0b111,0b001,0b111,0b100,0b111}, // 2
    {0b111,0b001,0b111,0b001,0b111}, // 3
    {0b101,0b101,0b111,0b001,0b001}, // 4
    {0b111,0b100,0b111,0b001,0b111}, // 5
    {0b111,0b100,0b111,0b101,0b111}, // 6
    {0b111,0b001,0b010,0b010,0b010}, // 7
    {0b111,0b101,0b111,0b101,0b111}, // 8
    {0b111,0b101,0b111,0b001,0b111}, // 9
};
static const uint8_t glyph_F[5]   = {0b111,0b100,0b111,0b100,0b100};
static const uint8_t glyph_P[5]   = {0b111,0b101,0b111,0b100,0b100};
static const uint8_t glyph_S[5]   = {0b111,0b100,0b111,0b001,0b111};
static const uint8_t glyph_M[5]   = {0b101,0b111,0b111,0b101,0b101};
static const uint8_t glyph_dot[5] = {0b000,0b000,0b000,0b000,0b010};

static void draw_glyph(uint32_t *fb, int fbstride, int fbw, int fbh,
                       const uint8_t rows[5], int x, int y, int scale,
                       uint32_t fg)
{
    for (int r = 0; r < 5; ++r)
    {
        for (int c = 0; c < 3; ++c)
        {
            if (!((rows[r] >> (2 - c)) & 1)) continue;
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx)
                {
                    int px = x + c * scale + sx;
                    int py = y + r * scale + sy;
                    if (px < 0 || py < 0 || px >= fbw || py >= fbh) continue;
                    fb[py * fbstride + px] = fg;
                }
        }
    }
}

static void draw_text(uint32_t *fb, int fbstride, int fbw, int fbh,
                      const char *s, int x, int y, int scale, uint32_t fg)
{
    int adv = 4 * scale; // 3px glyph + 1px gap
    for (; *s; ++s)
    {
        const uint8_t *g = NULL;
        if (*s >= '0' && *s <= '9') g = glyph_digit[*s - '0'];
        else if (*s == 'F') g = glyph_F;
        else if (*s == 'P') g = glyph_P;
        else if (*s == 'S') g = glyph_S;
        else if (*s == 'M') g = glyph_M;
        else if (*s == '.') g = glyph_dot;
        if (g) draw_glyph(fb, fbstride, fbw, fbh, g, x, y, scale, fg);
        if (*s != ' ' || true) x += adv;
    }
}

static void fps_record_and_draw(void)
{
    uint64_t now = mono_ns();
    if (fps_last_ns != 0)
    {
        uint64_t dt_ns = now - fps_last_ns;
        uint32_t u100 = (uint32_t)(dt_ns / 100000ULL); // 100us units
        if (u100 > 0xFFFF) u100 = 0xFFFF;
        frame_time_us[fps_head] = (uint16_t)u100;
        fps_head = (fps_head + 1) % FPS_HIST;
    }
    fps_last_ns = now;

    if (fps_window_start_ns == 0) fps_window_start_ns = now;
    ++fps_window_frames;
    uint64_t windowNs = now - fps_window_start_ns;
    if (windowNs >= 500000000ULL)
    {
        fps_value = (int)((uint64_t)fps_window_frames * 1000000000ULL / windowNs);
        // Mean frame time ms over history
        uint64_t sum = 0;
        int n = 0;
        for (int i = 0; i < FPS_HIST; ++i)
        {
            if (frame_time_us[i])
            {
                sum += frame_time_us[i];
                ++n;
            }
        }
        fps_mean_ms = (n > 0) ? ((float)sum / (float)n) / 10.0f : 0.0f;
        fps_window_frames = 0;
        fps_window_start_ns = now;
    }

    if (!fb_pixels || fb_width == 0 || fb_height == 0) return;

    const int scale = 2;
    const int gh = 5 * scale;
    const int pad = 4;
    const uint32_t white = 0x00FFFFFF;
    const uint32_t bg    = 0x00000000;

    // Panel size: text line (~80px) above graph (FPS_HIST px wide, 20px tall).
    const int panelW = FPS_HIST + pad * 2;
    const int panelH = gh + 2 + 20 + pad * 2;
    if ((int)fb_width < panelW + pad || (int)fb_height < panelH + pad) return;
    int px = (int)fb_width - panelW - pad;
    int py = pad;

    // Clear panel
    for (int yy = py; yy < py + panelH; ++yy)
    {
        uint32_t *row = fb_pixels + yy * fb_width;
        for (int xx = px; xx < px + panelW; ++xx) row[xx] = bg;
    }

    // "FPS NNN" + "MM.M"
    char buf[24];
    int fps = fps_value;
    if (fps < 0) fps = 0;
    if (fps > 999) fps = 999;
    int ms_int = (int)fps_mean_ms;
    int ms_dec = (int)((fps_mean_ms - (float)ms_int) * 10.0f);
    if (ms_dec < 0) ms_dec = 0; if (ms_dec > 9) ms_dec = 9;
    snprintf(buf, sizeof(buf), "FPS %d %d.%dMS", fps, ms_int, ms_dec);
    draw_text(fb_pixels, fb_width, fb_width, fb_height, buf,
              px + pad, py + pad, scale, white);

    // Bar graph: 20px tall, full-scale = 33ms.
    int gx = px + pad;
    int gy = py + pad + gh + 2;
    const int graphH = 20;
    for (int i = 0; i < FPS_HIST; ++i)
    {
        int idx = (fps_head + i) % FPS_HIST;
        uint32_t u100 = frame_time_us[idx];
        // Height: 330 units (33ms) = full
        int bar = (u100 * graphH) / 330;
        if (bar > graphH) bar = graphH;
        uint32_t col =
            (u100 > 330) ? 0x00FF3030 :
            (u100 > 200) ? 0x00FFC040 :
            0x0040C0FF;
        int cx = gx + i;
        if (cx >= (int)fb_width) break;
        for (int r = 0; r < graphH; ++r)
        {
            int pyy = gy + graphH - 1 - r;
            if (pyy < 0 || pyy >= (int)fb_height) continue;
            fb_pixels[pyy * fb_width + cx] = (r < bar) ? col : 0x00202020;
        }
    }
}

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
    int width = 320;
    int height = 240;

    // Look up the requested mode from vid_modes[] (defined in vid_brook.c).
    // Fall back to 320x240 if the mode number is out of range.
    if (!VID_GetModeInfo(&width, &height, mode))
    {
        ri.Con_Printf(PRINT_ALL, "SWimp_SetMode: invalid mode %d, using 320x240\n", mode);
        width = 320;
        height = 240;
    }

    // Ask the kernel to resize our VFB so we render 1:1 (no up/down scale).
    if (fb_fd >= 0)
    {
        struct fb_var_screeninfo vinfo;
        memset(&vinfo, 0, sizeof(vinfo));
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0)
        {
            vinfo.xres = width;
            vinfo.yres = height;
            vinfo.xres_virtual = width;
            vinfo.yres_virtual = height;
            if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo) == 0)
            {
                // Re-query the accepted size (kernel clamps to physical FB).
                if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0)
                {
                    // Drop old mmap and re-map at the new size.
                    if (fb_pixels && fb_pixels != MAP_FAILED)
                    {
                        munmap(fb_pixels, fb_width * fb_height * 4);
                        fb_pixels = NULL;
                    }

                    fb_width  = vinfo.xres;
                    fb_height = vinfo.yres;

                    size_t fb_size = (size_t)fb_width * fb_height * (vinfo.bits_per_pixel / 8);
                    fb_pixels = (uint32_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE,
                                                  MAP_SHARED, fb_fd, 0);
                    if (fb_pixels == MAP_FAILED)
                    {
                        ri.Con_Printf(PRINT_ALL, "SWimp_SetMode: re-mmap failed\n");
                        fb_pixels = NULL;
                    }
                }
            }
        }
    }

    // Clamp to framebuffer so we don't overrun the blit.
    if (fb_width && width > (int)fb_width) width = fb_width;
    if (fb_height && height > (int)fb_height) height = fb_height;

    ri.Con_Printf(PRINT_ALL, "SWimp_SetMode: %dx%d (mode %d) fb=%ux%u\n",
                  width, height, mode, fb_width, fb_height);

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

    // Notify client of the new window size (sets viddef.width/height)
    ri.Vid_NewWindow(width, height);

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

    // Frame diagnostics overlay (drawn after the scaled blit, so it's always
    // on top of the Q2 image).
    fps_record_and_draw();
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

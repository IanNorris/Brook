// glassdemo.c — Live frosted-glass control panel.
//
// Creates a window and gives it two sliders that tweak ITS OWN window's
// translucency (opacity) and backdrop blur in real time, via the per-window
// property syscall (WM_SET_WINDOW_PROPERTIES, 0xB00). Because the window is its
// own glass, dragging a slider visibly re-frosts the very panel you're dragging.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define BROOK_WM_CREATE_WINDOW          506
#define BROOK_WM_DESTROY_WINDOW         507
#define BROOK_WM_SIGNAL_DIRTY           508
#define BROOK_WM_POP_INPUT              510
#define BROOK_WM_SET_WINDOW_PROPERTIES  0xB00
#define WM_PROP_OPACITY                 (1u << 0)
#define WM_PROP_BLUR                    (1u << 1)

#define WM_EVT_CLOSE_REQUESTED          0x80

struct wm_create_out { uint32_t wm_id; uint32_t vfb_stride; uint64_t vfb_user; };
struct wm_input_evt {
    uint8_t type;   // 0=KeyPress 1=KeyRel 2=MouseMove 3=BtnDn 4=BtnUp 5=Scroll
    uint8_t scan; uint8_t ascii; uint8_t mods;
    int16_t x; int16_t y; uint32_t reserved;
};

static long wm_create(uint16_t w, uint16_t h, const char* t, struct wm_create_out* o) {
    return syscall(BROOK_WM_CREATE_WINDOW, (long)w, (long)h, (long)t, (long)o);
}
static long wm_signal_dirty(uint32_t id) { return syscall(BROOK_WM_SIGNAL_DIRTY, (long)id); }
static long wm_pop(uint32_t id, struct wm_input_evt* b, long m) {
    return syscall(BROOK_WM_POP_INPUT, (long)id, (long)b, m);
}
static long wm_destroy(uint32_t id) { return syscall(BROOK_WM_DESTROY_WINDOW, (long)id); }
static long wm_set_props(uint32_t id, unsigned mask, unsigned op, unsigned blur) {
    return syscall(BROOK_WM_SET_WINDOW_PROPERTIES, (long)id, (long)mask, (long)op, (long)blur);
}

// ---- compact 5x7 font (uppercase + digits + a few symbols) -----------------
// Each glyph is 7 rows; bit 4 (0x10) is the leftmost of 5 columns.
static const uint8_t FONT_SPACE[7] = {0,0,0,0,0,0,0};
static const uint8_t FONT_PCT[7]   = {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13};
static const uint8_t FONT_DIGIT[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};
// Letters we actually use: A B C I L O P R T U Y
static const uint8_t FONT_A[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
static const uint8_t FONT_B[7]={0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
static const uint8_t FONT_C[7]={0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
static const uint8_t FONT_I[7]={0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
static const uint8_t FONT_L[7]={0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
static const uint8_t FONT_O[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
static const uint8_t FONT_P[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
static const uint8_t FONT_R[7]={0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
static const uint8_t FONT_T[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
static const uint8_t FONT_U[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
static const uint8_t FONT_Y[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04};

static const uint8_t* glyph(char c) {
    if (c >= '0' && c <= '9') return FONT_DIGIT[c - '0'];
    switch (c) {
        case 'A': return FONT_A; case 'B': return FONT_B; case 'C': return FONT_C;
        case 'I': return FONT_I; case 'L': return FONT_L; case 'O': return FONT_O;
        case 'P': return FONT_P; case 'R': return FONT_R; case 'T': return FONT_T;
        case 'U': return FONT_U; case 'Y': return FONT_Y; case '%': return FONT_PCT;
        default:  return FONT_SPACE;
    }
}

// ---- drawing ----------------------------------------------------------------
struct fb { uint32_t* px; uint32_t stride; int w; int h; };

static void fill(struct fb* f, int x, int y, int w, int h, uint32_t argb) {
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= f->h) continue;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || xx >= f->w) continue;
            f->px[yy * f->stride + xx] = argb;
        }
    }
}

// Draw a string at (x,y) scaled by `s`. Returns the x past the text.
static int text(struct fb* f, int x, int y, const char* str, int s, uint32_t col) {
    for (const char* p = str; *p; ++p) {
        const uint8_t* g = glyph(*p);
        for (int row = 0; row < 7; ++row)
            for (int c = 0; c < 5; ++c)
                if (g[row] & (0x10 >> c))
                    fill(f, x + c * s, y + row * s, s, s, col);
        x += 6 * s;   // 5px glyph + 1px gap
    }
    return x;
}

static void utoa3(unsigned v, char* out) {  // up to 3 digits, no leading zeros
    char tmp[4]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 3) { tmp[n++] = '0' + (v % 10); v /= 10; }
    int o = 0; while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

// Layout constants.
#define WIN_W 400
#define WIN_H 230
#define TRK_X0 28
#define TRK_X1 (WIN_W - 28)
#define TRK_W  (TRK_X1 - TRK_X0)
#define OPA_Y  78
#define BLR_Y  168
#define TRK_H  10
#define HANDLE_W 16
#define HANDLE_H 28

#define OPA_MIN 40
#define OPA_MAX 255
#define BLR_MAX 16

static const uint32_t COL_PANEL   = 0xFF1B2433; // panel base (shows blurred behind it via window opacity)
static const uint32_t COL_TRACK   = 0xFF3A4658;
static const uint32_t COL_OPAFILL = 0xFF66B2FF;
static const uint32_t COL_BLRFILL = 0xFF7FE0C0;
static const uint32_t COL_HANDLE  = 0xFFF2F6FF;
static const uint32_t COL_HSHADOW = 0xFF0E1722;
static const uint32_t COL_LABEL   = 0xFFCFE2FF;
static const uint32_t COL_VALUE   = 0xFFFFFFFF;

static int opa_to_x(unsigned op) {
    if (op < OPA_MIN) op = OPA_MIN;
    return TRK_X0 + (int)((op - OPA_MIN) * (unsigned)TRK_W / (OPA_MAX - OPA_MIN));
}
static int blr_to_x(unsigned b) {
    if (b > BLR_MAX) b = BLR_MAX;
    return TRK_X0 + (int)(b * (unsigned)TRK_W / BLR_MAX);
}
static unsigned x_to_opa(int x) {
    if (x < TRK_X0) x = TRK_X0;
    if (x > TRK_X1) x = TRK_X1;
    return OPA_MIN + (unsigned)((x - TRK_X0) * (OPA_MAX - OPA_MIN) / TRK_W);
}
static unsigned x_to_blr(int x) {
    if (x < TRK_X0) x = TRK_X0;
    if (x > TRK_X1) x = TRK_X1;
    return (unsigned)((x - TRK_X0) * BLR_MAX / TRK_W);
}

static void draw_slider(struct fb* f, int trkY, int handleX,
                        uint32_t fillCol, const char* label, unsigned value) {
    text(f, TRK_X0, trkY - 26, label, 2, COL_LABEL);
    char num[4]; utoa3(value, num);
    text(f, TRK_X1 - (int)strlen(num) * 12, trkY - 26, num, 2, COL_VALUE);
    // Track + filled portion.
    fill(f, TRK_X0, trkY, TRK_W, TRK_H, COL_TRACK);
    fill(f, TRK_X0, trkY, handleX - TRK_X0, TRK_H, fillCol);
    // Handle (with a 1px drop shadow for a tactile feel).
    int hx = handleX - HANDLE_W / 2;
    int hy = trkY + TRK_H / 2 - HANDLE_H / 2;
    fill(f, hx + 1, hy + 1, HANDLE_W, HANDLE_H, COL_HSHADOW);
    fill(f, hx, hy, HANDLE_W, HANDLE_H, COL_HANDLE);
    fill(f, hx + HANDLE_W / 2 - 1, hy + 4, 2, HANDLE_H - 8, fillCol);
}

static void redraw(struct fb* f, unsigned opacity, unsigned blur) {
    fill(f, 0, 0, f->w, f->h, COL_PANEL);
    text(f, TRK_X0, 16, "GLASS", 3, COL_VALUE);
    draw_slider(f, OPA_Y, opa_to_x(opacity), COL_OPAFILL, "OPACITY", opacity);
    draw_slider(f, BLR_Y, blr_to_x(blur),    COL_BLRFILL, "BLUR",    blur);
}

int main(void) {
    struct wm_create_out info;
    if (wm_create(WIN_W, WIN_H, "Glass Controls", &info) != 0) return 1;

    struct fb f = { (uint32_t*)(uintptr_t)info.vfb_user, info.vfb_stride, WIN_W, WIN_H };
    unsigned opacity = 210, blur = 6;

    // Make this window glass to begin with (its own opacity + backdrop blur).
    wm_set_props(info.wm_id, WM_PROP_OPACITY | WM_PROP_BLUR, opacity, blur);
    redraw(&f, opacity, blur);
    wm_signal_dirty(info.wm_id);

    struct wm_input_evt buf[32];
    int btn_down = 0;        // mouse button currently held
    int active = 0;          // which slider is being dragged: 0 none, 1 opacity, 2 blur
    int alive = 1;

    while (alive) {
        long n = wm_pop(info.wm_id, buf, 32);
        int changed = 0;
        for (long i = 0; i < n; ++i) {
            struct wm_input_evt* e = &buf[i];
            if (e->type == WM_EVT_CLOSE_REQUESTED) { alive = 0; break; }
            if (e->type == 3) {           // button down — pick the slider under the cursor
                btn_down = 1;
                if (e->y >= OPA_Y - HANDLE_H && e->y <= OPA_Y + HANDLE_H)      active = 1;
                else if (e->y >= BLR_Y - HANDLE_H && e->y <= BLR_Y + HANDLE_H) active = 2;
                else active = 0;
            } else if (e->type == 4) {    // button up
                btn_down = 0; active = 0;
            }
            if (btn_down && active && (e->type == 2 || e->type == 3)) {
                if (active == 1) { unsigned v = x_to_opa(e->x); if (v != opacity) { opacity = v; changed = 1; } }
                else             { unsigned v = x_to_blr(e->x); if (v != blur)    { blur = v;    changed = 1; } }
            }
        }
        if (changed) {
            wm_set_props(info.wm_id, WM_PROP_OPACITY | WM_PROP_BLUR, opacity, blur);
            redraw(&f, opacity, blur);
            wm_signal_dirty(info.wm_id);
        }
        usleep(16 * 1000);   // ~60 Hz poll
    }

    wm_destroy(info.wm_id);
    printf("glassdemo: exit\n");
    return 0;
}

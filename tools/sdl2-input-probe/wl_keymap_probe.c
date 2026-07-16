/* End-to-end Wayland keymap probe for BRO-216 (yquake2 console text dead).
 *
 * The earlier xkb_memfd_probe fabricates its OWN keymap + memfd, so it only
 * tests mmap+compile in isolation. It cannot see the two things that are
 * actually different for a real client under waylandd:
 *   1. the real keymap STRING waylandd sends (make_keymap_fd in waylandd.c), and
 *   2. the real memfd fd delivered cross-process via SCM_RIGHTS.
 *
 * This probe is a minimal raw-libwayland client (no SDL, no GL, no game) that
 * connects to $WAYLAND_DISPLAY (waylandd), binds wl_seat, creates a
 * wl_keyboard, and in the keymap handler runs SDL3's EXACT path
 * (SDL_waylandevents.c:keyboard_handle_keymap, SDL 3.4.x):
 *
 *     map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
 *     keymap = xkb_keymap_new_from_string(ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
 *     state  = xkb_state_new(keymap);
 *
 * then, like keyboard_input_get_text(), for a set of test keycodes:
 *     xkb_state_key_get_syms(state, key+8, &syms) == 1
 *     xkb_keysym_to_utf8(syms[0], buf, 8) > 0
 *
 * yquake2's console text is produced ONLY when that last step yields text; its
 * scancodes/bindings come from a FIXED evdev->scancode table that needs no xkb
 * (SDL_GetScancodeFromTable(SDL_SCANCODE_TABLE_XFREE86_2, ...)). So text dead +
 * bindings working == SDL3's xkb.state is NULL/invalid. This probe reports the
 * exact step that fails, using the REAL keymap + REAL fd.
 *
 * Decision tree (run on Brook: WAYLAND_DISPLAY=... <out>/bin/wl_keymap_probe):
 *   no KEYMAP event         => waylandd never sent wl_keyboard.keymap (seat/kbd
 *                              creation path, or client got NO_KEYMAP)
 *   KEYMAP fd < 0           => SCM_RIGHTS hand-off delivered a bad fd (kernel
 *                              sys_recvmsg / UnixFdSnap install)
 *   FAIL mmap               => Brook MAP_PRIVATE memfd mmap broken
 *   bytes_match=0 / no NUL  => mmap returns wrong/zero bytes (memfd fault path)
 *   FAIL new_from_string    => real keymap string does not compile on Brook
 *   FAIL state_new          => keymap ok but state alloc failed
 *   syms!=1 / utf8<=0        => compiled but text extraction fails (the yq2 gate)
 *   PASS all                => the whole kernel+waylandd+xkb path is clean; the
 *                              BRO-216 failure is inside SDL3/sdl2-compat itself
 *                              (focus surface, TextInputActive, or seat wiring)
 *
 * Linux baseline (against any real compositor): PASS.
 *
 * Build: nix-build tools/sdl2-input-probe --no-out-link
 */
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static struct wl_seat *g_seat;
static struct wl_keyboard *g_kbd;
static struct xkb_context *g_ctx;
static int g_got_keymap;
static int g_exit;

/* evdev keycodes (= wl key + 8) for a spread of printable keys we expect to
 * produce text from the US keymap waylandd ships. */
struct { const char *name; uint32_t evdev; } g_test[] = {
    { "a",     38 }, { "s", 39 }, { "d", 40 },
    { "1",     10 }, { "space", 65 }, { "z", 52 },
};

static void run_sdl3_path(int fd, uint32_t size)
{
    if (fd < 0) {
        printf("FAIL: KEYMAP event delivered fd=%d (SCM_RIGHTS hand-off broken)\n", fd);
        g_exit = 1;
        return;
    }
    printf("KEYMAP event: fd=%d size=%u\n", fd, size);

    /* SDL3's exact mmap call (SDL_waylandevents.c:1684). */
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        printf("FAIL: mmap(NULL, %u, PROT_READ, MAP_PRIVATE, fd, 0)  <-- Brook memfd mmap\n", size);
        close(fd);
        g_exit = 1;
        return;
    }
    printf("mmap ok: first16='%.16s' nul_at_end=%d\n",
           map, size > 0 && map[size - 1] == 0);

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        g_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!km) {
        printf("FAIL: xkb_keymap_new_from_string (real keymap does not compile)\n");
        g_exit = 1;
        return;
    }

    /* SDL3 checks is_virtual = layout 0 has no name (SDL_waylandevents.c:1760).
     * If true, the fixed scancode table is skipped -- report it either way. */
    const char *l0 = xkb_keymap_layout_get_name(km, 0);
    printf("keymap compiled: num_layouts=%u layout0_name=%s is_virtual=%d\n",
           xkb_keymap_num_layouts(km), l0 ? l0 : "(null)", l0 == NULL);

    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        printf("FAIL: xkb_state_new\n");
        xkb_keymap_unref(km);
        g_exit = 1;
        return;
    }

    int all_ok = 1;
    for (size_t i = 0; i < sizeof(g_test) / sizeof(g_test[0]); i++) {
        const xkb_keysym_t *syms;
        int nsyms = xkb_state_key_get_syms(st, g_test[i].evdev, &syms);
        char buf[8] = {0};
        int n = (nsyms == 1) ? xkb_keysym_to_utf8(syms[0], buf, sizeof buf) : 0;
        printf("  key %-6s evdev=%2u nsyms=%d utf8='%s' n=%d %s\n",
               g_test[i].name, g_test[i].evdev, nsyms, buf, n,
               (nsyms == 1 && n > 0) ? "OK" : "NO-TEXT");
        if (!(nsyms == 1 && n > 0))
            all_ok = 0;
    }

    xkb_state_unref(st);
    xkb_keymap_unref(km);

    if (all_ok)
        printf("PASS: real keymap+fd compiled and produced text for all test keys.\n"
               "      Kernel/waylandd/xkb path is clean -> BRO-216 is inside SDL3/sdl2-compat.\n");
    else
        printf("PARTIAL: keymap compiled but some keys yielded no text (see above).\n");
    g_exit = 1;
}

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t size)
{
    (void)d; (void)k;
    g_got_keymap = 1;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        printf("FAIL: keymap format=%u (not XKB_V1 -> waylandd sent NO_KEYMAP)\n", fmt);
        if (fd >= 0) close(fd);
        g_exit = 1;
        return;
    }
    run_sdl3_path(fd, size);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf,
                     struct wl_array *keys) { (void)d;(void)k;(void)s;(void)sf;(void)keys; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf)
                     { (void)d;(void)k;(void)s;(void)sf; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key,
                   uint32_t st) { (void)d;(void)k;(void)s;(void)t;(void)key;(void)st; }
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat,
                    uint32_t lo, uint32_t grp) { (void)d;(void)k;(void)s;(void)dep;(void)lat;(void)lo;(void)grp; }
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t del)
                     { (void)d;(void)k;(void)r;(void)del; }

static const struct wl_keyboard_listener kb_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_kbd) {
        g_kbd = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_kbd, &kb_listener, NULL);
        printf("got wl_keyboard (seat caps=0x%x)\n", caps);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d;(void)s;(void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver)
{
    (void)d;
    printf("  global: %s v%u\n", iface, ver);
    if (strcmp(iface, "wl_seat") == 0) {
        uint32_t use = ver < 5 ? ver : 5;
        g_seat = wl_registry_bind(reg, name, &wl_seat_interface, use);
        wl_seat_add_listener(g_seat, &seat_listener, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d;(void)r;(void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

int main(void)
{
    /* Retry the connect a few times: when launched right after waylandd from an
     * rc, the socket may not be listening yet. Harmless on a live desktop. */
    struct wl_display *dpy = NULL;
    for (int i = 0; i < 50 && !dpy; i++) {
        dpy = wl_display_connect(NULL);
        if (!dpy) usleep(200 * 1000);
    }
    if (!dpy) { printf("FAIL: wl_display_connect (WAYLAND_DISPLAY=%s)\n",
                        getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
                return 1; }

    g_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!g_ctx) { printf("FAIL: xkb_context_new\n"); return 1; }

    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);

    /* Pump the display until we have a seat -> keyboard -> keymap, or we run
     * out of spins. A single roundtrip is not enough: some compositors
     * advertise wl_seat and its capabilities across several event batches. */
    int spins = 0;
    while (!g_exit && spins++ < 100) {
        if (wl_display_roundtrip(dpy) < 0) { printf("FAIL: display error\n"); break; }
        if (g_got_keymap) break;
        if (g_seat && !g_kbd) continue;      /* waiting for seat caps */
        if (!g_seat && spins > 5) {
            printf("FAIL: no wl_seat advertised by compositor\n");
            wl_display_disconnect(dpy);
            return 1;
        }
    }

    if (!g_got_keymap) {
        printf("FAIL: no wl_keyboard.keymap event received "
               "(waylandd seat/keyboard path, or no keyboard capability)\n");
        wl_display_disconnect(dpy);
        return 1;
    }
    wl_display_disconnect(dpy);
    return 0;
}

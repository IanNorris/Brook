/* xkbspy — LD_PRELOAD interposer for libxkbcommon, to pin BRO-216 inside SDL3.
 *
 * kbdprobe on Brook shows SDL_KEYDOWN with valid syms but ZERO SDL_TEXTINPUT.
 * The headless wl_keymap_probe already proved the keymap COMPILES and text
 * EXTRACTS from the real keymap fd in isolation. So the failure is how SDL3
 * (which yquake2 reaches via sdl2-compat) drives libxkbcommon. Rebuilding SDL3
 * is heavy; instead this tiny .so interposes the exact libxkbcommon entry
 * points SDL3's keyboard_handle_keymap + keyboard_input_get_text use, logging
 * each call + result to stderr (Brook routes stderr to serial). It forwards to
 * the real symbols via dlsym(RTLD_NEXT), so behaviour is unchanged.
 *
 * Wire it in by adding to the kbdprobe wrapper:
 *     export LD_PRELOAD=/nix/.../lib/xkbspy.so
 *
 * Reading the XKBSPY lines answers, in one run, exactly how far SDL3 gets:
 *   no "keymap_new_from_string"      => SDL never processed wl_keyboard.keymap
 *   "keymap_new_from_string -> NULL" => SDL's compile of the real keymap failed
 *   "state_new -> NULL"              => keymap ok but xkb state alloc failed
 *   compile+state OK, but on keypress no "key_get_syms" => SDL bailed BEFORE the
 *        text path (seat->keyboard.focus or SDL_TextInputActive gate) -> the bug
 *        is in SDL's focus/text-active wiring, NOT xkb
 *   "key_get_syms -> n" then "keysym_to_utf8 -> 0/empty" => text extraction fails
 *   "keysym_to_utf8 -> 'a'" but still no SDL_TEXTINPUT => SDL drops the text
 *        AFTER extraction (modifier/IME gate)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

/* Opaque xkb types — we only pass pointers through. */
struct xkb_context;
struct xkb_keymap;
struct xkb_state;
typedef uint32_t xkb_keysym_t;
typedef uint32_t xkb_keycode_t;

static void spy(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("XKBSPY: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

#define REAL(sym) \
    static typeof(&sym) real; \
    if (!real) real = (typeof(&sym))resolve(#sym)

/* Brook's dynamic loader does not resolve dlsym(RTLD_NEXT) for symbols that are
 * already DT_NEEDED (it null-returns, and calling that crashes at RIP=0). So
 * resolve against an explicit dlopen handle for the real libxkbcommon instead —
 * it is already loaded (DT_NEEDED by SDL3), so this returns the live handle and
 * finds the genuine symbols. Fall back to RTLD_DEFAULT/NEXT if the open fails. */
static void *xkb_handle(void)
{
    static void *h;
    static int tried;
    if (!tried) {
        tried = 1;
        h = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        if (!h) h = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_GLOBAL);
        spy("xkbspy: dlopen(libxkbcommon.so.0) -> %p (%s)", h, h ? "ok" : dlerror());
    }
    return h;
}

static void *resolve(const char *name)
{
    void *h = xkb_handle();
    void *p = h ? dlsym(h, name) : NULL;
    if (!p) p = dlsym(RTLD_DEFAULT, name);   /* last resort */
    if (!p) spy("xkbspy: FAILED to resolve %s (interposer would crash)", name);
    return p;
}

struct xkb_context *xkb_context_new(int flags)
{
    REAL(xkb_context_new);
    if (!real) return NULL;
    struct xkb_context *r = real(flags);
    spy("xkb_context_new(flags=%d) -> %p", flags, (void *)r);
    return r;
}

struct xkb_keymap *xkb_keymap_new_from_string(struct xkb_context *ctx,
                                              const char *s, int format, int flags)
{
    REAL(xkb_keymap_new_from_string);
    if (!real) return NULL;
    struct xkb_keymap *r = real(ctx, s, format, flags);
    size_t len = s ? strlen(s) : 0;
    char head[17] = {0};
    if (s) memcpy(head, s, len < 16 ? len : 16);
    spy("xkb_keymap_new_from_string(ctx=%p, len=%zu, fmt=%d, flags=%d) -> %p first16='%s'",
        (void *)ctx, len, format, flags, (void *)r, head);
    return r;
}

struct xkb_keymap *xkb_keymap_new_from_names(struct xkb_context *ctx,
                                             const void *names, int flags)
{
    REAL(xkb_keymap_new_from_names);
    if (!real) return NULL;
    struct xkb_keymap *r = real(ctx, names, flags);
    spy("xkb_keymap_new_from_names(ctx=%p, flags=%d) -> %p", (void *)ctx, flags, (void *)r);
    return r;
}

struct xkb_state *xkb_state_new(struct xkb_keymap *km)
{
    REAL(xkb_state_new);
    if (!real) return NULL;
    struct xkb_state *r = real(km);
    spy("xkb_state_new(keymap=%p) -> %p", (void *)km, (void *)r);
    return r;
}

int xkb_state_key_get_syms(struct xkb_state *st, xkb_keycode_t key,
                           const xkb_keysym_t **syms)
{
    REAL(xkb_state_key_get_syms);
    if (!real) return 0;
    int n = real(st, key, syms);
    static int count;
    if (count++ < 40)
        spy("xkb_state_key_get_syms(state=%p, key=%u) -> nsyms=%d sym0=0x%x",
            (void *)st, key, n, (n > 0 && syms && *syms) ? (*syms)[0] : 0);
    return n;
}

int xkb_keysym_to_utf8(xkb_keysym_t sym, char *buf, size_t size)
{
    REAL(xkb_keysym_to_utf8);
    if (!real) return 0;
    int n = real(sym, buf, size);
    static int count;
    if (count++ < 40)
        spy("xkb_keysym_to_utf8(sym=0x%x) -> n=%d utf8='%s'", sym, n,
            (n > 0 && buf) ? buf : "");
    return n;
}

int xkb_state_key_get_utf8(struct xkb_state *st, xkb_keycode_t key,
                           char *buf, size_t size)
{
    REAL(xkb_state_key_get_utf8);
    if (!real) return 0;
    int n = real(st, key, buf, size);
    static int count;
    if (count++ < 40)
        spy("xkb_state_key_get_utf8(state=%p, key=%u) -> n=%d utf8='%s'",
            (void *)st, key, n, (n > 0 && buf) ? buf : "");
    return n;
}

/* --- Compose interposition: distinguishes the SDL3 compose path (returns
 * COMPOSING and swallows text) from the SDL_USE_IME path. If these are called,
 * SDL built a compose state from the locale; get_status returning COMPOSING (1)
 * on a plain key is the smoking gun for text being swallowed. --- */
struct xkb_compose_table;
struct xkb_compose_state;

struct xkb_compose_table *xkb_compose_table_new_from_locale(
    struct xkb_context *ctx, const char *locale, int flags)
{
    REAL(xkb_compose_table_new_from_locale);
    if (!real) return NULL;
    struct xkb_compose_table *r = real(ctx, locale, flags);
    spy("xkb_compose_table_new_from_locale(locale='%s', flags=%d) -> %p",
        locale ? locale : "(null)", flags, (void *)r);
    return r;
}

struct xkb_compose_state *xkb_compose_state_new(struct xkb_compose_table *t, int flags)
{
    REAL(xkb_compose_state_new);
    if (!real) return NULL;
    struct xkb_compose_state *r = real(t, flags);
    spy("xkb_compose_state_new(table=%p, flags=%d) -> %p", (void *)t, flags, (void *)r);
    return r;
}

int xkb_compose_state_feed(struct xkb_compose_state *st, xkb_keysym_t sym)
{
    REAL(xkb_compose_state_feed);
    if (!real) return 0;
    int r = real(st, sym);
    static int count;
    if (count++ < 40)
        spy("xkb_compose_state_feed(state=%p, sym=0x%x) -> %d (0=IGNORED,1=ACCEPTED)",
            (void *)st, sym, r);
    return r;
}

int xkb_compose_state_get_status(struct xkb_compose_state *st)
{
    REAL(xkb_compose_state_get_status);
    if (!real) return 0;
    int r = real(st);
    static int count;
    if (count++ < 40)
        spy("xkb_compose_state_get_status(state=%p) -> %d "
            "(0=NOTHING,1=COMPOSING,2=COMPOSED,3=CANCELLED) %s",
            (void *)st, r, r == 1 ? "<== SWALLOWS TEXT" : "");
    return r;
}

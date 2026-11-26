#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *shell;
static struct wl_surface *surface;
static struct wl_buffer *buffer;
static struct xkb_state *state;
static const uint32_t expected_gb[] = {'"', 0xa3, '@', '#', '~', 0x5c, '|', 0x20ac, '|'};
static const uint32_t expected_us[] = {'@', '#', '"', 0x5c, '|', '<', '>', '4', 0x60};
static const uint32_t *expected;
static unsigned count;

static void globals(void *data, struct wl_registry *r, uint32_t id, const char *name, uint32_t version)
{
    (void)data;
    (void)version;
    if (!strcmp(name, wl_compositor_interface.name)) compositor = wl_registry_bind(r,id,&wl_compositor_interface,1);
    if (!strcmp(name, wl_shm_interface.name)) shm = wl_registry_bind(r,id,&wl_shm_interface,1);
    if (!strcmp(name, wl_seat_interface.name)) seat = wl_registry_bind(r,id,&wl_seat_interface,1);
    if (!strcmp(name, xdg_wm_base_interface.name)) shell = wl_registry_bind(r,id,&xdg_wm_base_interface,1);
}
static void removed(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static void ping(void *d, struct xdg_wm_base *s, uint32_t serial) { (void)d; xdg_wm_base_pong(s,serial); }
static void configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    wl_surface_attach(surface,buffer,0,0);
    wl_surface_damage(surface,0,0,256,128);
    wl_surface_commit(surface);
}
static void top_configure(void *d,struct xdg_toplevel *top,int32_t w,int32_t h,struct wl_array *states)
{ (void)d;(void)top;(void)w;(void)h;(void)states; }
static void top_close(void *d,struct xdg_toplevel *top) { (void)d;(void)top; exit(2); }

static void keymap(void *d,struct wl_keyboard *k,uint32_t format,int32_t fd,uint32_t size)
{
    (void)d;(void)k;
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size);
    char *text = mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);
    assert(text != MAP_FAILED);
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *map = xkb_keymap_new_from_string(context,text,XKB_KEYMAP_FORMAT_TEXT_V1,0);
    assert(map);
    state = xkb_state_new(map);
    assert(state);
    xkb_keymap_unref(map);
    xkb_context_unref(context);
    munmap(text,size);
    close(fd);
}
static void enter(void *d,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s,struct wl_array *keys)
{ (void)d;(void)k;(void)serial;(void)s;(void)keys; puts("INPUT_PREFS_FOCUSED"); }
static void leave(void *d,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s)
{ (void)d;(void)k;(void)serial;(void)s; }
static void key(void *d,struct wl_keyboard *k,uint32_t serial,uint32_t time,uint32_t code,uint32_t down)
{
    (void)d;(void)k;(void)serial;(void)time;
    if (!down || !state) return;
    const uint32_t cp = xkb_state_key_get_utf32(state, code + 8);
    if (!cp) return;
    printf("INPUT_PREFS_KEY code=%u unicode=%u\n", code, cp);
    assert(count < sizeof(expected_gb)/sizeof(*expected_gb) && cp == expected[count]);
    if (++count == sizeof(expected_gb)/sizeof(*expected_gb)) puts("INPUT_PREFS_KEYS_PASS");
}
static void mods(void *d,struct wl_keyboard *k,uint32_t serial,uint32_t depressed,uint32_t latched,uint32_t locked,uint32_t group)
{ (void)d;(void)k;(void)serial; if (state) xkb_state_update_mask(state,depressed,latched,locked,0,0,group); }

static void pointer_enter(void *d,struct wl_pointer *p,uint32_t serial,struct wl_surface *s,wl_fixed_t x,wl_fixed_t y)
{ (void)d;(void)p;(void)serial;(void)s;(void)x;(void)y; }
static void pointer_leave(void *d,struct wl_pointer *p,uint32_t serial,struct wl_surface *s)
{ (void)d;(void)p;(void)serial;(void)s; }
static void motion(void *d,struct wl_pointer *p,uint32_t stamp,wl_fixed_t x,wl_fixed_t y)
{ (void)d;(void)p;(void)stamp;(void)x;(void)y; puts("INPUT_PREFS_CLIENT_MOTION"); }
static void button(void *d,struct wl_pointer *p,uint32_t serial,uint32_t time,uint32_t key,uint32_t down)
{ (void)d;(void)p;(void)serial;(void)time;(void)key;(void)down; }
static void axis(void *d,struct wl_pointer *p,uint32_t time,uint32_t a,wl_fixed_t v)
{ (void)d;(void)p;(void)time;(void)a;(void)v; }

int main(int argc,char **argv)
{
    assert(argc == 3);
    expected = !strcmp(argv[1],"gb") ? expected_gb : expected_us;
    setvbuf(stdout,NULL,_IONBF,0);
    assert(getenv("XCURSOR_SIZE") && !strcmp(getenv("XCURSOR_SIZE"),argv[2]));
    struct wl_display *display = NULL;
    for (unsigned i=0; !display && i<240; ++i) {
        display = wl_display_connect(NULL);
        if (!display) usleep(50000);
    }
    assert(display);
    struct wl_registry *registry = wl_display_get_registry(display);
    static const struct wl_registry_listener registry_listener = { globals, removed };
    wl_registry_add_listener(registry,&registry_listener,NULL);
    assert(wl_display_roundtrip(display) >= 0);
    assert(compositor && shm && seat && shell);

    const int width=256,height=128,bytes=width*height*4;
    const int fd=memfd_create("input-preferences",0);
    assert(fd >= 0 && ftruncate(fd,bytes) == 0);
    const uint32_t pixel=0xff225544;
    for (int i=0;i<bytes/4;++i) assert(write(fd,&pixel,4)==4);
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,bytes);
    buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    static const struct wl_keyboard_listener keyboard_listener = {
        .keymap=keymap,.enter=enter,.leave=leave,.key=key,.modifiers=mods,
    };
    static const struct wl_pointer_listener pointer_listener = {
        .enter=pointer_enter,.leave=pointer_leave,.motion=motion,.button=button,.axis=axis,
    };
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat),&keyboard_listener,NULL);
    wl_pointer_add_listener(wl_seat_get_pointer(seat),&pointer_listener,NULL);

    static const struct xdg_wm_base_listener shell_listener = { ping };
    static const struct xdg_surface_listener surface_listener = { configure };
    static const struct xdg_toplevel_listener top_listener = { .configure=top_configure,.close=top_close };
    xdg_wm_base_add_listener(shell,&shell_listener,NULL);
    surface=wl_compositor_create_surface(compositor);
    struct xdg_surface *xs=xdg_wm_base_get_xdg_surface(shell,surface);
    xdg_surface_add_listener(xs,&surface_listener,NULL);
    struct xdg_toplevel *top=xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top,&top_listener,NULL);
    xdg_toplevel_set_title(top,"Input preferences fixture");
    xdg_toplevel_set_app_id(top,"org.brook.InputPreferencesFixture");
    wl_surface_commit(surface);
    while (wl_display_dispatch(display) >= 0) {}
    return 1;
}

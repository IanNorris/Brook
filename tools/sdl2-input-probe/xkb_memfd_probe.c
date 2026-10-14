/* Headless replica of SDL2's Wayland keymap path (BRO-216), runnable from a
 * plain Brook terminal (no GUI, no input injection).
 *
 * SDL2's keyboard_handle_keymap does exactly:
 *     map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
 *     keymap = xkb_keymap_new_from_string(ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
 * where `fd` is a memfd the compositor wrote the keymap into. If Brook's
 * MAP_PRIVATE mmap of a memfd doesn't reflect the written bytes, or xkb compile
 * fails, SDL2's xkb.state stays NULL -> console text dead while scancodes work.
 *
 * This isolates that exact path in one process. PASS on Linux, run on Brook:
 *   FAIL at mmap        => Brook MAP_PRIVATE memfd mmap broken (kernel bug)
 *   FAIL at new_string  => Brook mmap returns wrong/zero bytes, or compile issue
 *   PASS                => the mmap+compile path is fine; the SDL2 failure is
 *                          elsewhere (SCM_RIGHTS fd hand-off, or SDL2 not
 *                          processing the wl_keyboard.keymap event)
 */
#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) { printf("FAIL: xkb_context_new\n"); return 1; }

    struct xkb_rule_names names = {
        .rules = "evdev", .model = "pc105", .layout = "us",
        .variant = "", .options = ""
    };
    struct xkb_keymap *km0 =
        xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km0) { printf("FAIL: new_from_names (no xkb data?)\n"); return 1; }
    char *str = xkb_keymap_get_as_string(km0, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t len = strlen(str) + 1;   /* include trailing NUL, like waylandd sizeof() */
    printf("keymap string len=%zu\n", len);

    int fd = (int)syscall(SYS_memfd_create, "probe-keymap", 0u);
    if (fd < 0) { printf("FAIL: memfd_create\n"); return 1; }
    if (ftruncate(fd, (off_t)len) != 0) { printf("FAIL: ftruncate\n"); return 1; }
    if (write(fd, str, len) != (ssize_t)len) { printf("FAIL: write memfd\n"); return 1; }

    /* SDL2's exact mmap call. */
    char *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { printf("FAIL: mmap(MAP_PRIVATE, memfd)\n"); return 2; }
    printf("mmap ok: first4='%.4s' nul_at_end=%d bytes_match=%d\n",
           map, map[len - 1] == 0, memcmp(map, str, len) == 0);

    struct xkb_keymap *km =
        xkb_keymap_new_from_string(ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        printf("FAIL: xkb_keymap_new_from_string(mmap MAP_PRIVATE)  <-- SDL2 PATH BROKE\n");
        return 3;
    }
    struct xkb_state *st = xkb_state_new(km);
    if (!st) { printf("FAIL: xkb_state_new\n"); return 4; }

    char buf[8] = {0};
    int n = xkb_state_key_get_utf8(st, 38 /* evdev 'a' + 8 */, buf, sizeof buf);
    printf("PASS: SDL2-path keymap compiled + state built; key38 utf8='%s' (n=%d)\n",
           buf, n);
    return 0;
}

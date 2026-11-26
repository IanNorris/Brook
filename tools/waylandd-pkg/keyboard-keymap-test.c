/* Compile and load the actual compositor map without connecting to a display. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XKBfile.h>
#include <X11/extensions/XKM.h>
#include "keyboard-keymap.h"

static void check_map(const char *xkbcomp, const char *keymap, int gb)
{
    char directory[] = "./brook-keymap-test-XXXXXX";
    assert(mkdtemp(directory));
    char source[PATH_MAX], compiled[PATH_MAX];
    assert(snprintf(source, sizeof(source), "%s/map.xkb", directory) > 0);
    assert(snprintf(compiled, sizeof(compiled), "%s/map.xkm", directory) > 0);
    FILE *file = fopen(source, "wb");
    assert(file);
    assert(fwrite(keymap, 1, strlen(keymap), file) == strlen(keymap));
    assert(fclose(file) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        execl(xkbcomp, xkbcomp, "-w", "0", "-xkm", source, compiled, (char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    XkbInitAtoms(NULL);
    file = fopen(compiled, "rb");
    assert(file);
    XkbFileInfo info = {0};
    /* Geometry is optional; required sections include the indicator map. */
    unsigned missing = XkmReadFile(file, 0x5f, 0x7f, &info);
    assert((missing & 0x5f) == 0 && (info.defined & 0x5f) == 0x5f);
    assert(fclose(file) == 0);
    assert(info.xkb && (info.defined & XkmIndicatorsMask));
    XkbDescPtr map = info.xkb;
    assert(map->indicators && map->names && map->map && map->server);
    const char *names[] = {"Caps Lock", "Num Lock", "Scroll Lock"};
    unsigned masks[] = {LockMask, Mod2Mask, Mod3Mask};
    for (unsigned i = 0; i < 3; ++i) {
        assert(map->names->indicators[i] == XkbInternAtom(NULL, names[i], False));
        assert(map->indicators->maps[i].which_mods == XkbIM_UseLocked);
        assert(map->indicators->maps[i].mods.real_mods == masks[i]);
        assert(!(map->indicators->maps[i].flags &
                 (XkbIM_NoAutomatic | XkbIM_LEDDrivesKB)));
    }
    /* Indicator metadata must preserve the ordinary US typing/chord map. */
    assert(XkbKeySymEntry(map, 38, 0, 0) == XK_a);
    assert(XkbKeySymEntry(map, 38, 1, 0) == XK_A);
    assert(XkbKeySymEntry(map, 37, 0, 0) == XK_Control_L);
    assert(XkbKeySymEntry(map, 105, 0, 0) == XK_Control_R);
    assert(map->map->modmap[37] & ControlMask);
    assert(map->map->modmap[105] & ControlMask);
    /* XKM stores compatibility interpretations; the server derives per-key
     * actions when applying them. Keep the default modifier-map action. */
    assert(map->compat);
    int modifier_action = 0;
    for (unsigned i = 0; i < map->compat->num_si; ++i) {
        XkbSymInterpretPtr interpretation = &map->compat->sym_interpret[i];
        XkbAction action;
        memcpy(&action, &interpretation->act, sizeof(action));
        if (interpretation->sym == NoSymbol && action.type == XkbSA_SetMods &&
            (action.mods.flags & XkbSA_UseModMapMods) &&
            (action.mods.flags & XkbSA_ClearLocks))
            modifier_action = 1;
    }
    assert(modifier_action);
    if (gb) {
        assert(XkbKeySymEntry(map, 11, 1, 0) == XK_quotedbl);
        assert(XkbKeySymEntry(map, 12, 1, 0) == XK_sterling);
        assert(XkbKeySymEntry(map, 13, 2, 0) == XK_EuroSign);
        assert(XkbKeySymEntry(map, 48, 1, 0) == XK_at);
        assert(XkbKeySymEntry(map, 51, 0, 0) == XK_numbersign);
        assert(XkbKeySymEntry(map, 51, 1, 0) == XK_asciitilde);
        assert(XkbKeySymEntry(map, 94, 0, 0) == XK_backslash);
        assert(XkbKeySymEntry(map, 94, 1, 0) == XK_bar);
        assert(XkbKeySymEntry(map, 108, 0, 0) == XK_ISO_Level3_Shift);
        assert(map->map->modmap[108] & Mod5Mask);
    } else {
        assert(XkbKeySymEntry(map, 11, 1, 0) == XK_at);
    }
    XkbFreeKeyboard(map, XkbAllComponentsMask, True);
    assert(unlink(source) == 0 && unlink(compiled) == 0 && rmdir(directory) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    check_map(argv[1], brook_xkb_keymap, 0);
    check_map(argv[1], brook_xkb_keymap_gb, 1);
    puts("KEYBOARD_KEYMAP_PASS layouts=us,gb");
    return 0;
}

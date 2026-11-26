#pragma once
#include "input.h"
#include "../../shared/input_preferences.h"

namespace brook {
static constexpr char KeyNormalUs[128] = {
    /* 0x00 */ 0,
    /* 0x01 Esc      */ 0x1B,
    /* 0x02 1        */ '1',
    /* 0x03 2        */ '2',
    /* 0x04 3        */ '3',
    /* 0x05 4        */ '4',
    /* 0x06 5        */ '5',
    /* 0x07 6        */ '6',
    /* 0x08 7        */ '7',
    /* 0x09 8        */ '8',
    /* 0x0A 9        */ '9',
    /* 0x0B 0        */ '0',
    /* 0x0C -        */ '-',
    /* 0x0D =        */ '=',
    /* 0x0E Bksp     */ '\b',
    /* 0x0F Tab      */ '\t',
    /* 0x10 Q        */ 'q',
    /* 0x11 W        */ 'w',
    /* 0x12 E        */ 'e',
    /* 0x13 R        */ 'r',
    /* 0x14 T        */ 't',
    /* 0x15 Y        */ 'y',
    /* 0x16 U        */ 'u',
    /* 0x17 I        */ 'i',
    /* 0x18 O        */ 'o',
    /* 0x19 P        */ 'p',
    /* 0x1A [        */ '[',
    /* 0x1B ]        */ ']',
    /* 0x1C Enter    */ '\n',
    /* 0x1D L-Ctrl   */ 0,
    /* 0x1E A        */ 'a',
    /* 0x1F S        */ 's',
    /* 0x20 D        */ 'd',
    /* 0x21 F        */ 'f',
    /* 0x22 G        */ 'g',
    /* 0x23 H        */ 'h',
    /* 0x24 J        */ 'j',
    /* 0x25 K        */ 'k',
    /* 0x26 L        */ 'l',
    /* 0x27 ;        */ ';',
    /* 0x28 '        */ '\'',
    /* 0x29 `        */ '`',
    /* 0x2A L-Shift  */ 0,
    /* 0x2B \        */ '\\',
    /* 0x2C Z        */ 'z',
    /* 0x2D X        */ 'x',
    /* 0x2E C        */ 'c',
    /* 0x2F V        */ 'v',
    /* 0x30 B        */ 'b',
    /* 0x31 N        */ 'n',
    /* 0x32 M        */ 'm',
    /* 0x33 ,        */ ',',
    /* 0x34 .        */ '.',
    /* 0x35 /        */ '/',
    /* 0x36 R-Shift  */ 0,
    /* 0x37 *        */ '*',
    /* 0x38 L-Alt    */ 0,
    /* 0x39 Space    */ ' ',
    /* 0x3A Caps     */ 0,
    /* 0x3B F1       */ 0,
    /* 0x3C F2       */ 0,
    /* 0x3D F3       */ 0,
    /* 0x3E F4       */ 0,
    /* 0x3F F5       */ 0,
    /* 0x40 F6       */ 0,
    /* 0x41 F7       */ 0,
    /* 0x42 F8       */ 0,
    /* 0x43 F9       */ 0,
    /* 0x44 F10      */ 0,
    /* 0x45 NumLk    */ 0,
    /* 0x46 ScrlLk   */ 0,
    /* 0x47 KP7      */ '7',
    /* 0x48 KP8 / Up */ '8',
    /* 0x49 KP9      */ '9',
    /* 0x4A KP-      */ '-',
    /* 0x4B KP4      */ '4',
    /* 0x4C KP5      */ '5',
    /* 0x4D KP6      */ '6',
    /* 0x4E KP+      */ '+',
    /* 0x4F KP1      */ '1',
    /* 0x50 KP2/Dn   */ '2',
    /* 0x51 KP3      */ '3',
    /* 0x52 KP0      */ '0',
    /* 0x53 KP.      */ '.',
    /* 0x54          */ 0,
    /* 0x55          */ 0,
    /* 0x56          */ 0,
    /* 0x57 F11      */ 0,
    /* 0x58 F12      */ 0,
    /* 0x59–0x7F     */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static constexpr char KeyShiftUs[128] = {
    /* 0x00 */ 0,
    /* 0x01 Esc */ 0x1B,
    /* 0x02 1 */ '!',
    /* 0x03 2 */ '@',
    /* 0x04 3 */ '#',
    /* 0x05 4 */ '$',
    /* 0x06 5 */ '%',
    /* 0x07 6 */ '^',
    /* 0x08 7 */ '&',
    /* 0x09 8 */ '*',
    /* 0x0A 9 */ '(',
    /* 0x0B 0 */ ')',
    /* 0x0C - */ '_',
    /* 0x0D = */ '+',
    /* 0x0E Bksp */ '\b',
    /* 0x0F Tab  */ '\t',
    /* 0x10 */ 'Q',
    /* 0x11 */ 'W',
    /* 0x12 */ 'E',
    /* 0x13 */ 'R',
    /* 0x14 */ 'T',
    /* 0x15 */ 'Y',
    /* 0x16 */ 'U',
    /* 0x17 */ 'I',
    /* 0x18 */ 'O',
    /* 0x19 */ 'P',
    /* 0x1A */ '{',
    /* 0x1B */ '}',
    /* 0x1C */ '\n',
    /* 0x1D */ 0,
    /* 0x1E */ 'A',
    /* 0x1F */ 'S',
    /* 0x20 */ 'D',
    /* 0x21 */ 'F',
    /* 0x22 */ 'G',
    /* 0x23 */ 'H',
    /* 0x24 */ 'J',
    /* 0x25 */ 'K',
    /* 0x26 */ 'L',
    /* 0x27 */ ':',
    /* 0x28 */ '"',
    /* 0x29 */ '~',
    /* 0x2A */ 0,
    /* 0x2B */ '|',
    /* 0x2C */ 'Z',
    /* 0x2D */ 'X',
    /* 0x2E */ 'C',
    /* 0x2F */ 'V',
    /* 0x30 */ 'B',
    /* 0x31 */ 'N',
    /* 0x32 */ 'M',
    /* 0x33 */ '<',
    /* 0x34 */ '>',
    /* 0x35 */ '?',
    /* 0x36 */ 0,
    /* 0x37 */ '*',
    /* 0x38 */ 0,
    /* 0x39 */ ' ',
    /* 0x3A–0x7F */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

struct KeyboardKeyLevels {
    uint8_t scan;
    uint16_t levels[4]; // plain, shift, AltGr, Shift+AltGr
};

inline constexpr KeyboardKeyLevels KeyOverridesUs[] = {
    {0x56, {'<', '>', '<', '>'}},
};
inline constexpr KeyboardKeyLevels KeyOverridesGb[] = {
    {0x03, {'2', '"', '2', '"'}},
    {0x04, {'3', 0x00a3, '3', 0x00a3}},
    {0x05, {'4', '$', 0x20ac, 0x20ac}},
    {0x28, {0x27, '@', 0x27, '@'}},
    {0x29, {0x60, 0x00ac, 0x00a6, 0}},
    {0x2b, {'#', '~', '#', '~'}},
    {0x56, {0x5c, '|', 0x5c, '|'}},
};

struct KeyboardLayoutData {
    const KeyboardKeyLevels* overrides;
    unsigned count;
};

inline constexpr KeyboardLayoutData KeyboardLayouts[] = {
    {KeyOverridesUs, sizeof(KeyOverridesUs) / sizeof(*KeyOverridesUs)},
    {KeyOverridesGb, sizeof(KeyOverridesGb) / sizeof(*KeyOverridesGb)},
};

constexpr uint16_t KeyboardCodepoint(uint32_t layout, uint8_t scan, uint8_t mods)
{
    if (scan >= 128) return 0;
    const auto& map = KeyboardLayouts[layout < sizeof(KeyboardLayouts)/sizeof(*KeyboardLayouts) ? layout : BROOK_KEYBOARD_US];
    const char base = KeyNormalUs[scan];
    bool shifted = (mods & INPUT_MOD_SHIFT) != 0;
    if ((mods & INPUT_MOD_CAPSLOCK) && base >= 'a' && base <= 'z') shifted = !shifted;
    const unsigned level = unsigned(shifted) | ((mods & INPUT_MOD_ALTGR) ? 2u : 0u);
    for (unsigned i = 0; i < map.count; ++i)
        if (map.overrides[i].scan == scan) return map.overrides[i].levels[level];
    return shifted ? KeyShiftUs[scan] : base;
}

inline unsigned KeyboardUtf8(uint16_t cp, char (&bytes)[3])
{
    if (!cp) return 0;
    if (cp < 0x80) {
        bytes[0] = char(cp);
        return 1;
    }
    if (cp < 0x800) {
        bytes[0] = char(0xc0 | cp >> 6);
        bytes[1] = char(0x80 | (cp & 0x3f));
        return 2;
    }
    bytes[0] = char(0xe0 | cp >> 12);
    bytes[1] = char(0x80 | ((cp >> 6) & 0x3f));
    bytes[2] = char(0x80 | (cp & 0x3f));
    return 3;
}
} // namespace brook

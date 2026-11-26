#include "../../kernel/src/keyboard_layout.h"
#include "../../kernel/src/cursor_composition.h"

#include <cassert>
#include <initializer_list>

int main()
{
    using namespace brook;
    for (const auto layout : {BROOK_KEYBOARD_US, BROOK_KEYBOARD_GB}) {
        assert(KeyboardCodepoint(layout, 0x02, INPUT_MOD_CAPSLOCK) == '1');
        assert(KeyboardCodepoint(layout, 0x02, INPUT_MOD_CAPSLOCK | INPUT_MOD_LSHIFT) == '!');
        assert(KeyboardCodepoint(layout, 0x1e, INPUT_MOD_CAPSLOCK) == 'A');
        assert(KeyboardCodepoint(layout, 0x1e, INPUT_MOD_CAPSLOCK | INPUT_MOD_LSHIFT) == 'a');
    }
    assert(KeyboardCodepoint(BROOK_KEYBOARD_US, 0x03, INPUT_MOD_RSHIFT) == '@');
    assert(KeyboardCodepoint(BROOK_KEYBOARD_GB, 0x03, INPUT_MOD_RSHIFT) == '"');
    assert(KeyboardCodepoint(BROOK_KEYBOARD_GB, 0x04, INPUT_MOD_LSHIFT) == 0x00a3);
    assert(KeyboardCodepoint(BROOK_KEYBOARD_GB, 0x28, INPUT_MOD_LSHIFT) == '@');
    assert(KeyboardCodepoint(BROOK_KEYBOARD_GB, 0x2b, 0) == '#');
    assert(KeyboardCodepoint(BROOK_KEYBOARD_GB, 0x2b, INPUT_MOD_LSHIFT) == '~');
    assert(KeyboardCodepoint(BROOK_KEYBOARD_GB, 0x56, 0) == 0x5c);
    assert(KeyboardCodepoint(BROOK_KEYBOARD_GB, 0x56, INPUT_MOD_LSHIFT) == '|');
    assert(KeyboardCodepoint(BROOK_KEYBOARD_GB, 0x05, INPUT_MOD_ALTGR) == 0x20ac);
    char bytes[3];
    assert(KeyboardUtf8(0x20ac, bytes) == 3 && uint8_t(bytes[0]) == 0xe2 &&
           uint8_t(bytes[1]) == 0x82 && uint8_t(bytes[2]) == 0xac);
    assert(brook_keyboard_layout_parse("gb\r\n", BROOK_KEYBOARD_US) == BROOK_KEYBOARD_GB);
    assert(brook_keyboard_layout_parse("gbx", BROOK_KEYBOARD_US) == BROOK_KEYBOARD_US);
    assert(brook_cursor_size_parse("48\r", 24) == 48);
    assert(brook_cursor_size_parse("37", 32) == 32);
    CursorImage source, scaled;
    ScaleCursor(source, scaled, CursorPreferencePercent(200, 48));
    assert(scaled.width == 48 && scaled.height == 64);
    ScaleCursor(source, scaled, CursorPreferencePercent(200, 64));
    assert(scaled.width == 64 && scaled.height == 86);
}

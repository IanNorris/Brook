#include "usb_boot_keyboard.h"
#include <cassert>
#include <vector>

using namespace brook;

struct Key {
    uint8_t scan;
    bool down;
    uint8_t mods;
};

int main()
{
    UsbBootKeyboard keyboard;
    uint8_t first[8]{}, second[8]{};
    std::vector<Key> events;
    const auto emit = [&](uint8_t scan, bool down, uint8_t mods) { events.push_back({scan, down, mods}); };
    const uint8_t held[8]{2, 0, 0x4b, 0x4b, 0x4a, 0x50};
    keyboard.Update(held, first, emit);
    assert(events.size() == 4);
    const uint8_t expected[]{42, SC_EXT_PGUP, SC_EXT_HOME, SC_EXT_LEFT};
    for (unsigned i = 0; i < 4; ++i)
        assert(events[i].scan == expected[i] && events[i].down && events[i].mods == INPUT_MOD_LSHIFT);

    keyboard.Update(held, second, emit);
    const uint8_t rollover[8]{0, 0, 1, 1, 1, 1, 1, 1};
    keyboard.Update(rollover, first, emit);
    const uint8_t empty[8]{};
    keyboard.Update(empty, second, emit);
    assert(events.size() == 4);

    keyboard.Update(empty, first, emit);
    assert(events.size() == 8);
    for (unsigned i = 0; i < 4; ++i)
        assert(events[i + 4].scan == expected[i] && !events[i + 4].down && !events[i + 4].mods);

    const uint8_t caps[8]{0, 0, 0x39};
    keyboard.Update(caps, first, emit);
    assert(events.back().scan == 58 && events.back().mods == INPUT_MOD_CAPSLOCK);
    assert(UsbBootScanCode[0x49] == SC_EXT_INSERT && UsbBootScanCode[0x4c] == SC_EXT_DELETE);
    assert(UsbBootScanCode[0x4d] == SC_EXT_END && UsbBootScanCode[0x4e] == SC_EXT_PGDN);
    assert(UsbBootScanCode[0x4f] == SC_EXT_RIGHT && UsbBootScanCode[0x51] == SC_EXT_DOWN);
    assert(UsbBootScanCode[0x52] == SC_EXT_UP);
}

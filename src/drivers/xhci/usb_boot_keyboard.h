#pragma once

#include "../../kernel/src/input.h"
#include "../../shared/input_preferences.h"

namespace brook {

// HID boot keyboard usages translated into the desktop scan-code namespace.
inline constexpr uint8_t UsbBootScanCode[128] = {
    0,   0,   0,   0,   30,  48,  46,  32,  // 0x00-0x07: None,None,None,None, A,B,C,D
    18,  33,  34,  35,  23,  36,  37,  38,  // 0x08-0x0F: E,F,G,H, I,J,K,L
    50,  49,  24,  25,  16,  19,  31,  20,  // 0x10-0x17: M,N,O,P, Q,R,S,T
    22,  47,  17,  45,  21,  44,  2,   3,   // 0x18-0x1F: U,V,W,X, Y,Z,1,2
    4,   5,   6,   7,   8,   9,   10,  11,  // 0x20-0x27: 3,4,5,6, 7,8,9,0
    28,  1,   14,  15,  57,  12,  13,  26,  // 0x28-0x2F: Enter,Esc,Bksp,Tab, Space,-,=,[
    27,  43,  43,  39,  40,  41,  51,  52,  // 0x30-0x37: ],\,\,;, ',`,,,.
    53,  58,  59,  60,  61,  62,  63,  64,  // 0x38-0x3F: /,CapsLk,F1,F2, F3,F4,F5,F6
    65,  66,  67,  68,  87,  88,  99,  70,  // 0x40-0x47: F7,F8,F9,F10, F11,F12,PrtSc,ScrLk
    119, SC_EXT_INSERT, SC_EXT_HOME, SC_EXT_PGUP, SC_EXT_DELETE, SC_EXT_END, SC_EXT_PGDN, SC_EXT_RIGHT, // 0x48-0x4F: Pause,Ins,Home,PgUp, Del,End,PgDn,Right
    SC_EXT_LEFT, SC_EXT_DOWN, SC_EXT_UP, 69,  98,  55,  74,  78,  // 0x50-0x57: Left,Down,Up,NumLk, KP/,KP*,KP-,KP+
    96,  79,  80,  81,  75,  76,  77,  71,  // 0x58-0x5F: KPEnt,KP1,KP2,KP3, KP4,KP5,KP6,KP7
    72,  73,  82,  83,  0x56,   0,   0,   0,   // 0x60-0x67: KP8,KP9,KP0,KP., unused...
    0,   0,   0,   0,   0,   0,   0,   0,   // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,   // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0,   // 0x78-0x7F
};

inline constexpr uint8_t UsbBootModifierScans[] = {29, 42, 56, SC_LSUPER, 97, 54, 100, SC_RSUPER};

class UsbBootKeyboard {
public:
    // Each interface supplies its last valid report. Aggregate held keys so
    // unplugging or releasing one keyboard cannot release another's keys.
    template<typename Emit>
    void Update(const uint8_t* report, uint8_t* previous, Emit&& emit, uint32_t layout = BROOK_KEYBOARD_US)
    {
        for (unsigned i = 2; i < 8; ++i)
            if (report[i] && report[i] < 4) return; // rollover or report error

        bool modifierChanged[8]{};
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint8_t mask = 1u << bit;
            if ((report[0] ^ previous[0]) & mask)
                modifierChanged[bit] = UpdateHeld(modifiers_[bit], report[0] & mask);
        }

        for (unsigned bit = 0; bit < 8; ++bit)
            if (modifierChanged[bit])
                emit(UsbBootModifierScans[bit], modifiers_[bit] != 0, Modifiers(layout));

        for (unsigned i = 2; i < 8; ++i) {
            const uint8_t key = previous[i];
            if (Mapped(key) && !Contains(previous, key, i) && !Contains(report, key) &&
                UpdateHeld(keys_[key], false))
                emit(UsbBootScanCode[key], false, Modifiers(layout));
        }
        for (unsigned i = 2; i < 8; ++i) {
            const uint8_t key = report[i];
            if (Mapped(key) && !Contains(report, key, i) && !Contains(previous, key) &&
                UpdateHeld(keys_[key], true)) {
                if (key == 0x39) capsLock_ = !capsLock_;
                emit(UsbBootScanCode[key], true, Modifiers(layout));
            }
        }
        for (unsigned i = 0; i < 8; ++i) previous[i] = report[i];
    }

private:
    static bool Contains(const uint8_t* report, uint8_t key, unsigned end = 8)
    {
        for (unsigned i = 2; i < end; ++i)
            if (report[i] == key) return true;
        return false;
    }

    static bool Mapped(uint8_t key) { return key < sizeof(UsbBootScanCode) && UsbBootScanCode[key]; }

    static bool UpdateHeld(uint8_t& count, bool pressed)
    {
        if (pressed) return ++count == 1;
        return count && --count == 0;
    }

    uint8_t Modifiers(uint32_t layout) const
    {
        return (modifiers_[1] ? INPUT_MOD_LSHIFT : 0) |
               (modifiers_[5] ? INPUT_MOD_RSHIFT : 0) |
               ((modifiers_[0] || modifiers_[4]) ? INPUT_MOD_CTRL : 0) |
               ((modifiers_[2] || (modifiers_[6] && layout == BROOK_KEYBOARD_US)) ? INPUT_MOD_ALT : 0) |
               (modifiers_[6] ? INPUT_MOD_ALTGR : 0) |
               ((modifiers_[3] || modifiers_[7]) ? INPUT_MOD_SUPER : 0) |
               (capsLock_ ? INPUT_MOD_CAPSLOCK : 0);
    }

    uint8_t keys_[sizeof(UsbBootScanCode)]{};
    uint8_t modifiers_[8]{};
    bool capsLock_ = false;
};

}

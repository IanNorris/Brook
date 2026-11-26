/* Each complete keymap uses the same evdev keycodes and modifier ordering. */
#ifndef BROOK_KEYBOARD_KEYMAP_H
#define BROOK_KEYBOARD_KEYMAP_H
#define BROOK_KEYMAP_GB 0
static const char brook_xkb_keymap[] =
#include "keyboard-keymap.inc"
;
#undef BROOK_KEYMAP_GB
#define BROOK_KEYMAP_GB 1
static const char brook_xkb_keymap_gb[] =
#include "keyboard-keymap.inc"
;
#undef BROOK_KEYMAP_GB
#endif

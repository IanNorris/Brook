#ifndef BROOK_INPUT_PREFERENCES_H
#define BROOK_INPUT_PREFERENCES_H
#include <stdint.h>

#define BROOK_SYS_INPUT_PREFERENCES 0xB2A
#define BROOK_INPUT_PREF_KEYBOARD 0u
#define BROOK_INPUT_PREF_CURSOR 1u

#define BROOK_KEYBOARD_US 0u
#define BROOK_KEYBOARD_GB 1u
#define BROOK_CURSOR_SIZE_DEFAULT 24u

static inline int brook_input_tail(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    return !*text;
}

static inline uint32_t brook_keyboard_layout_parse(const char *text, uint32_t fallback)
{
    if (!text[0] || !text[1] || !brook_input_tail(text + 2)) return fallback;
    if (text[0] == 'g' && text[1] == 'b') return BROOK_KEYBOARD_GB;
    if (text[0] == 'u' && text[1] == 's') return BROOK_KEYBOARD_US;
    return fallback;
}

static inline const char *brook_keyboard_layout_name(uint32_t layout)
{
    return layout == BROOK_KEYBOARD_GB ? "gb" : "us";
}

static inline int brook_cursor_size_valid(uint32_t size)
{
    return size == 24 || size == 32 || size == 48 || size == 64;
}

static inline uint32_t brook_cursor_size_parse(const char *text, uint32_t fallback)
{
    if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9' ||
        !brook_input_tail(text + 2)) return fallback;
    const uint32_t size = (text[0] - '0') * 10 + (text[1] - '0');
    return brook_cursor_size_valid(size) ? size : fallback;
}
#endif

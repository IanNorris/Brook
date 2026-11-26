#pragma once

// WM_CREATE_WINDOW opts a window into uncaptured fine scroll. In a fine
// WM_POP_INPUT scroll event, bytes 4..5 are vertical v120 (up positive) and
// bytes 6..7 are horizontal v120 (right positive). Bytes 8..11 preserve the
// local pointer position as int16 x then y. Legacy scroll keeps x/y at 4..7
// and the optional capture token at 8..11.
#define BROOK_WM_CREATE_FLAG_FINE_SCROLL 8u
#define BROOK_WM_CREATE_FLAG_CAPTURE_EXCLUDED 16u
#define BROOK_SYS_WM_EXCLUDE_CAPTURE 0xB28
#define BROOK_WM_POLICY_TITLE_BYTES 4096
#define BROOK_WM_SCROLL_FINE    0x20u
#define BROOK_WM_SCROLL_FINGER  0x40u
#define BROOK_WM_SCROLL_END     0x80u

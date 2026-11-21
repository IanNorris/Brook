#pragma once
#include <stdint.h>

#define BROOK_CAPTION_STRIP_MAX_ROWS 96u
#define BROOK_CAPTION_BUTTON_LOGICAL_WIDTH 48

// Optional row palette sent to WM_SET_DECORATION_MODE. Entries are RGB in
// physical output rows, from the client's top rim to its tab-strip divider.
typedef struct BrookCaptionStrip {
    uint32_t count;
    uint32_t rgb[BROOK_CAPTION_STRIP_MAX_ROWS];
} BrookCaptionStrip;

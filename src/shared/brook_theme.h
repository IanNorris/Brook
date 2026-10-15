#ifndef BROOK_THEME_H
#define BROOK_THEME_H
#include <stdint.h>

/* Shared by kernel chrome and the Settings app's actual-palette preview.
 * Packed colors are 0x00RRGGBB. Palettes are immutable; settings persist IDs. */
enum {
    BROOK_THEME_BROOK = 0,
    BROOK_THEME_DAWN = 1,
    BROOK_THEME_MIDNIGHT = 2,
    BROOK_THEME_COUNT = 3
};
typedef struct BrookThemePalette {
    uint32_t titleActive;
    uint32_t titleInactive;
    uint32_t text;
    uint32_t borderActive;
    uint32_t borderInactive;
    uint32_t closeHover;
    uint32_t closeText;
    uint32_t taskbar;
    uint32_t taskButton;
    uint32_t taskActive;
    uint32_t taskText;
    uint32_t secondaryText;
    uint32_t mutedText;
    uint32_t captionHover;
    uint32_t taskHover;
    uint32_t separator;
    uint32_t minimized;
    uint32_t itemHover;
    uint32_t launcher;
    uint32_t launcherBorder;
    uint32_t gpu3d;
    uint32_t gpu2d;
    uint32_t gpuInactive;
} BrookThemePalette;

static inline const BrookThemePalette* brook_theme_palette(uint32_t id)
{
    static const BrookThemePalette palettes[BROOK_THEME_COUNT] = {
        { /* Brook */
            0x002D3B4Du, /* titleActive */
            0x00292E36u, /* titleInactive */
            0x00EDF2F7u, /* text */
            0x006F9CC5u, /* borderActive */
            0x00434B57u, /* borderInactive */
            0x00B64048u, /* closeHover */
            0x00FFFFFFu, /* closeText */
            0x001C232Du, /* taskbar */
            0x002B3542u, /* taskButton */
            0x003C526Bu, /* taskActive */
            0x00E0E7EFu, /* taskText */
            0x00BAC8D8u, /* secondaryText */
            0x009DA9B7u, /* mutedText */
            0x00455466u, /* captionHover */
            0x003C4859u, /* taskHover */
            0x00414E60u, /* separator */
            0x00778596u, /* minimized */
            0x003D4D62u, /* itemHover */
            0x00232C38u, /* launcher */
            0x004B5A6Eu, /* launcherBorder */
            0x007FC9E8u, /* gpu3d */
            0x0083C99Bu, /* gpu2d */
            0x007E8B9Bu, /* gpuInactive */
        },
        { /* Dawn */
            0x00F5EDE0u, /* titleActive */
            0x00E8E6E1u, /* titleInactive */
            0x00282C34u, /* text */
            0x00B98045u, /* borderActive */
            0x00C9C4BBu, /* borderInactive */
            0x00B64048u, /* closeHover */
            0x00FFFFFFu, /* closeText */
            0x00F2EDE5u, /* taskbar */
            0x00E3DED4u, /* taskButton */
            0x00EAD0AAu, /* taskActive */
            0x002D343Fu, /* taskText */
            0x00596575u, /* secondaryText */
            0x0074777Du, /* mutedText */
            0x00E7D5BCu, /* captionHover */
            0x00E5DAC9u, /* taskHover */
            0x00C9BBA7u, /* separator */
            0x008E7E69u, /* minimized */
            0x00EBE0CFu, /* itemHover */
            0x00FFFAF1u, /* launcher */
            0x00D1C5B4u, /* launcherBorder */
            0x00246780u, /* gpu3d */
            0x00327044u, /* gpu2d */
            0x0074777Du, /* gpuInactive */
        },
        { /* Midnight */
            0x00171F33u, /* titleActive */
            0x00151923u, /* titleInactive */
            0x00EDEDFAu, /* text */
            0x00A68EE8u, /* borderActive */
            0x00343C50u, /* borderInactive */
            0x00B13C5Cu, /* closeHover */
            0x00FFFFFFu, /* closeText */
            0x000B101Bu, /* taskbar */
            0x00182136u, /* taskButton */
            0x003B315Bu, /* taskActive */
            0x00DEDFF3u, /* taskText */
            0x00B0B7D6u, /* secondaryText */
            0x00909AB0u, /* mutedText */
            0x002C3350u, /* captionHover */
            0x002A3047u, /* taskHover */
            0x00323C54u, /* separator */
            0x00756B9Du, /* minimized */
            0x0030304Bu, /* itemHover */
            0x00141B2Au, /* launcher */
            0x00414360u, /* launcherBorder */
            0x0082C8E4u, /* gpu3d */
            0x0085CBAAu, /* gpu2d */
            0x007F8AA4u, /* gpuInactive */
        },
    };
    return id < BROOK_THEME_COUNT ? &palettes[id] : 0;
}

static inline const char* brook_theme_name(uint32_t id)
{
    static const char* const names[BROOK_THEME_COUNT] = {"Brook", "Dawn", "Midnight"};
    return id < BROOK_THEME_COUNT ? names[id] : "Unknown";
}
#endif

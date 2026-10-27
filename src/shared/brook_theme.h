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
            0x0028466Fu, /* titleActive */
            0x00253851u, /* titleInactive */
            0x00E5EDF8u, /* text */
            0x00D0DEF0u, /* borderActive */
            0x0034455Fu, /* borderInactive */
            0x00B64048u, /* closeHover */
            0x00FFFFFFu, /* closeText */
            0x0016273Fu, /* taskbar */
            0x00243A56u, /* taskButton */
            0x00355987u, /* taskActive */
            0x00E5EDF8u, /* taskText */
            0x00A4B2C7u, /* secondaryText */
            0x00899BB6u, /* mutedText */
            0x003F6488u, /* captionHover */
            0x002E4664u, /* taskHover */
            0x003C5270u, /* separator */
            0x007086A4u, /* minimized */
            0x002C496Du, /* itemHover */
            0x001C314Cu, /* launcher */
            0x007899C8u, /* launcherBorder */
            0x0068ACFFu, /* gpu3d */
            0x008CBAFFu, /* gpu2d */
            0x00708198u, /* gpuInactive */
        },
        { /* Dawn */
            0x00D4E3F5u, /* titleActive */
            0x00D0D9E4u, /* titleInactive */
            0x00213950u, /* text */
            0x00FFFFFFu, /* borderActive */
            0x00A2B3C7u, /* borderInactive */
            0x00B64048u, /* closeHover */
            0x00FFFFFFu, /* closeText */
            0x00E3EBF6u, /* taskbar */
            0x00D3DFEFu, /* taskButton */
            0x00B5CEE9u, /* taskActive */
            0x0025364Bu, /* taskText */
            0x00526783u, /* secondaryText */
            0x006F7F95u, /* mutedText */
            0x00BDD1EAu, /* captionHover */
            0x00CCDCEEu, /* taskHover */
            0x00B1C1D8u, /* separator */
            0x007A92AEu, /* minimized */
            0x00C7D9EFu, /* itemHover */
            0x00EDF3FBu, /* launcher */
            0x00B9CCE5u, /* launcherBorder */
            0x00377CC0u, /* gpu3d */
            0x004C87BAu, /* gpu2d */
            0x006F8099u, /* gpuInactive */
        },
        { /* Midnight */
            0x0024364Fu, /* titleActive */
            0x001E2B40u, /* titleInactive */
            0x00E6EDF8u, /* text */
            0x00BECDE3u, /* borderActive */
            0x0035445Bu, /* borderInactive */
            0x00B13C5Cu, /* closeHover */
            0x00FFFFFFu, /* closeText */
            0x00101B2Bu, /* taskbar */
            0x001E2E44u, /* taskButton */
            0x00354C6Cu, /* taskActive */
            0x00E4EDF8u, /* taskText */
            0x00A4B2C7u, /* secondaryText */
            0x008596AEu, /* mutedText */
            0x003E5575u, /* captionHover */
            0x002C3E58u, /* taskHover */
            0x00394E6Bu, /* separator */
            0x006A83A5u, /* minimized */
            0x002E4563u, /* itemHover */
            0x0019283Du, /* launcher */
            0x00617C9Fu, /* launcherBorder */
            0x0068ACFFu, /* gpu3d */
            0x008CBAFFu, /* gpu2d */
            0x00708198u, /* gpuInactive */
        },
    };
    return id < BROOK_THEME_COUNT ? &palettes[id] : 0;
}

static inline const char* brook_theme_name(uint32_t id)
{
    static const char* const names[BROOK_THEME_COUNT] = {"Clearwater", "Opal", "Smoked"};
    return id < BROOK_THEME_COUNT ? names[id] : "Unknown";
}
#endif

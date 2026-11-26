#pragma once

// Display scale shared by Settings, the WM and the display server.
#define BROOK_DESKTOP_SCALE_MIN 100u
#define BROOK_DESKTOP_SCALE_MAX 200u
#define BROOK_DESKTOP_SCALE_STEP 5u
#define BROOK_DESKTOP_SCALE_COUNT 21u

#ifdef __cplusplus
#define BROOK_SCALE_INLINE constexpr
#else
#define BROOK_SCALE_INLINE inline
#endif
static BROOK_SCALE_INLINE int brook_desktop_scale_valid(unsigned percent)
{
    return percent >= BROOK_DESKTOP_SCALE_MIN && percent <= BROOK_DESKTOP_SCALE_MAX &&
           (percent - BROOK_DESKTOP_SCALE_MIN) % BROOK_DESKTOP_SCALE_STEP == 0;
}
#undef BROOK_SCALE_INLINE

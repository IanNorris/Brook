#pragma once
// Zero-copy step 2: WM_SET_NV_LAYER(wmId, renderD130 fd, map offset, stride, width | height << 16, flags).
// flags bit 0 attaches the client area of window wmId to the GART BO at `map offset` (the
// BO's GEM_INFO map_handle plus the byte offset of the first pixel) in the caller's
// renderD130 file: stride bytes per row, width x height 32-bit pixels, shown opaque. The BO
// pages are referenced until the layer is replaced or detached. flags 0 detaches (other
// arguments ignored). The call returns after no display copy reads the previous layer, so
// its wl_buffer may be released right away. Attaching publishes the window like a VFB commit.
#define BROOK_SYS_WM_SET_NV_LAYER 0xB2B
#define BROOK_WM_NV_LAYER_ATTACH 1u

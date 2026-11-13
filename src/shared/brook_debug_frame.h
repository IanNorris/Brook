#ifndef BROOK_DEBUG_FRAME_H
#define BROOK_DEBUG_FRAME_H
#include <stdint.h>
enum { BROOK_SYS_DEBUG_FRAME=0xB0E, BROOK_DEBUG_FRAME_CHUNK=65536,
       BROOK_DEBUG_FRAME_BGRA8=1 };
/* Root-only, owner-bound immutable completed-frame lease. No kernel addresses.
 * syscall(op, token, destination, length, offset, 0):
 * 0=request token; 1=metadata (exact struct size); 2=bounded read; 3=release.
 * State matches DebugFrameState: Pending=1, Ready=2. A lease expires after30s.
 * Packed rows, byte order B,G,R,A. Pixels include the completed desktop frame,
 * not a promise of hardware cursor-plane contents. */
struct BrookDebugFrameInfo {
    uint32_t token,state,width,height,stride,format;
    uint64_t bytes,tick_ms,epoch;
};
/* Optional op 4: extended status for the same owner-bound token. The source
 * describes the pixels copied at capture time, not the current scanout. */
enum { BROOK_DEBUG_FRAME_SOURCE_FRONT=1, BROOK_DEBUG_FRAME_SOURCE_LEASED=2,
       BROOK_DEBUG_FRAME_SOURCE_LEGACY_GPU=3 };
struct BrookDebugFrameInfoV2 {
    struct BrookDebugFrameInfo frame;
    uint32_t source,reserved;
};
/* Debug-frame op 5 returns immutable capture provenance. Mode describes the
 * most recent successful leased publication copied into this capture; it is
 * NONE for software and legacy GPU sources. It does not assert panel scanout. */
enum { BROOK_DEBUG_FRAME_PRESENT_NONE=0, BROOK_DEBUG_FRAME_PRESENT_FULL=1,
       BROOK_DEBUG_FRAME_PRESENT_PARTIAL=2 };
struct BrookDebugFrameInfoV3 {
    struct BrookDebugFrameInfoV2 v2;
    uint32_t presentation_mode,reserved;
};
/* Overlay syscall op 4 returns the disjoint semantic row bands of the same
 * immutable capture lease. Coordinates refer to the full packed image; use
 * op 2 to read the corresponding rows. At most seven bands are returned.
 * Op 5 reads one complete band into packed rows: destination and length
 * describe the exact output size, and offset selects the band index. A
 * failed call may have written a prefix; discard the entire destination. */
enum { BROOK_DESKTOP_OVERLAY_READ_BAND=5 };
enum { BROOK_DESKTOP_OVERLAY_MAX_BANDS=7 };
struct BrookDesktopOverlayBand {uint32_t x,y,width,height;};
struct BrookDesktopOverlayBands {
    uint32_t count,reserved;
    struct BrookDesktopOverlayBand bands[BROOK_DESKTOP_OVERLAY_MAX_BANDS];
};
#endif

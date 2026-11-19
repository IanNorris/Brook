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
/* Op 6 (optional; older kernels return -EINVAL) requests a capture like op
 * 0 with options: destination/length describe one BrookDebugFrameRequest
 * (exact length). CURSOR composites the visible hardware cursor plane over
 * the image (a software cursor is already part of the pixels).
 * WINDOW_BUFFER copies the current client buffer of the named window
 * (unoccluded, buffer pixels, no chrome) instead of the screen; it fails
 * with -ENOENT for an unknown window and -EOPNOTSUPP without a CPU buffer.
 * Op 7 fills BrookDebugFrameInfoV4 (exact length) for the same token.
 * Sources 4..7 extend the V2 source field: on Dell builds full-screen
 * captures read the surface primary plane A is fetching (DSPASURFLIVE), so
 * a renderer image, kernel flip buffer or direct client buffer replaces
 * the GOP framebuffer whenever it is the one on the panel. */
enum { BROOK_DEBUG_FRAME_REQUEST=6, BROOK_DEBUG_FRAME_INFO_V4=7 };
enum { BROOK_DEBUG_FRAME_FLAG_CURSOR=1, BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER=2 };
struct BrookDebugFrameRequest {
    uint32_t flags,window_id;
    uint64_t window_incarnation;
};
enum { BROOK_DEBUG_FRAME_SOURCE_FLIP=4, BROOK_DEBUG_FRAME_SOURCE_ZERO_COPY=5,
       BROOK_DEBUG_FRAME_SOURCE_DIRECT_CLIENT=6, BROOK_DEBUG_FRAME_SOURCE_WINDOW_BUFFER=7 };
/* What primary plane A was fetching when the pixels were copied. UNREAD:
 * no plane readback on this build/device (the GOP or VM framebuffer is the
 * display). UNKNOWN: an unrecognised surface; the GOP was copied instead. */
enum { BROOK_DEBUG_FRAME_PLANE_UNREAD=0, BROOK_DEBUG_FRAME_PLANE_GOP=1, BROOK_DEBUG_FRAME_PLANE_FLIP=2,
       BROOK_DEBUG_FRAME_PLANE_ZERO_COPY=3, BROOK_DEBUG_FRAME_PLANE_CLIENT=4, BROOK_DEBUG_FRAME_PLANE_UNKNOWN=5 };
enum { BROOK_DEBUG_FRAME_INFO_CURSOR_COMPOSITED=1, BROOK_DEBUG_FRAME_INFO_DETILED=2,
       BROOK_DEBUG_FRAME_INFO_CURSOR_PLANE_VISIBLE=4, BROOK_DEBUG_FRAME_INFO_SWAPPED_RB=8 };
struct BrookDebugFrameInfoV4 {
    struct BrookDebugFrameInfoV3 v3;
    uint32_t plane,plane_surface,plane_control,plane_stride;
    uint32_t flags,window_id;
    /* Window captures: the buffer's integer upscale on screen (1 = 1:1). */
    uint32_t window_scale,copy_us;
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
/* Overlay change tracking (all optional; older kernels return -EINVAL).
 * Op 6 returns the current overlay content generation (> 0). It changes
 * whenever any taskbar, launcher or boot-summary pixel changes; the pointer
 * is excluded. Querying keeps the compositor's change detection armed for a
 * few seconds; detection runs on input and at least every 100 ms.
 * Op 0 with offset=BROOK_DESKTOP_OVERLAY_NO_CURSOR requests a capture
 * without the pointer (the caller draws it from op 8).
 * Op 7 blocks while the token is pending, for at most offset milliseconds
 * (<= 250), then fills BrookDesktopOverlayInfo (exact length). generation
 * is the content generation of a Ready capture; state may still be pending.
 * Op 8 fills BrookDesktopOverlayCursor (exact length). offset is the serial
 * the caller already holds: when it matches, pixels are left unwritten.
 * x/y are the top-left of the image (pointer minus hotspot); pixels are
 * straight-alpha B,G,R,A rows of stride BROOK_DESKTOP_CURSOR_MAX. */
enum { BROOK_DESKTOP_OVERLAY_GENERATION=6, BROOK_DESKTOP_OVERLAY_WAIT=7,
       BROOK_DESKTOP_OVERLAY_CURSOR=8 };
enum { BROOK_DESKTOP_OVERLAY_NO_CURSOR=1, BROOK_DESKTOP_OVERLAY_WAIT_MAX_MS=250 };
enum { BROOK_DESKTOP_CURSOR_MAX=64 };
/* Op 9 (optional; older kernels return -EINVAL) is op 4 plus, per band, an
 * opaque bit and a content key, for the same capture lease. Bit i of opaque
 * means every pixel of band i has alpha 255 (a shell surface drawn in an
 * opaque material), so a renderer may let it replace whatever lies beneath.
 * keys[i] is a nonzero hash of band i's geometry and pixels: a band with the
 * same geometry and key as one the caller holds has identical pixels, so it
 * need not be reread or redrawn. bands.reserved stays zero. */
enum { BROOK_DESKTOP_OVERLAY_BANDS_V2=9 };
struct BrookDesktopOverlayBandsV2 {
    struct BrookDesktopOverlayBands bands;
    uint32_t opaque,reserved;
    uint64_t keys[BROOK_DESKTOP_OVERLAY_MAX_BANDS];
};
/* Op 10 (optional; older kernels return -EINVAL): the shell surface records
 * (layers ABI v2, struct BrookDesktopSurface in desktop_layers_abi.h) of the
 * same capture lease, back to front: kind, rectangle, material, opacity,
 * radii, shadow and content key. The pointer is not a surface. */
enum { BROOK_DESKTOP_OVERLAY_SURFACES=10, BROOK_DESKTOP_OVERLAY_MAX_SURFACES=4 };
struct BrookDesktopOverlayInfo {
    struct BrookDebugFrameInfo frame;
    uint64_t generation;
};
/* Final syscall argument for op 8. V3 returns a positive epoch once the
 * hardware plane has latched its transparent probe and needs a clean
 * primary frame. Older kernels reject V3 with EINVAL; retry with zero
 * to keep the software cursor. */
#define BROOK_DESKTOP_CURSOR_PLANE_V1 1
#define BROOK_DESKTOP_CURSOR_PLANE_V2 2
#define BROOK_DESKTOP_CURSOR_PLANE_V3 3
/* After presenting that frame, pass the epoch in the offset argument. */
enum { BROOK_DESKTOP_OVERLAY_CURSOR_PRESENTED=11 };
struct BrookDesktopOverlayCursor {
    uint32_t visible,width,height,stride;
    int32_t x,y;
    uint64_t serial;
    uint32_t pixels[BROOK_DESKTOP_CURSOR_MAX*BROOK_DESKTOP_CURSOR_MAX];
};
#endif

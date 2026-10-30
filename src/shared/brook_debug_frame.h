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
#endif

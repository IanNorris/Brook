#pragma once
#include <stdint.h>
// Allocate a zeroed, CPU-unmappable Intel PRIME buffer for an experimental
// GPU output. Returns a close-on-exec fd or a negative Linux error. This is
// an allocation primitive: it grants no display or presentation ownership.
#define BROOK_SYS_GPU_OUTPUT_ALLOC 0xB17
#define BROOK_GPU_OUTPUT_MAX_BYTES (64ull*1024ull*1024ull)
// Experimental offscreen handoff only. The session does not own scanout.
// ctl(op, argument, 0, 0, 0, 0): attach accepts 0 (default) or an explicit
// supported depth, detach uses 0; reserve
// takes a protected output fd and returns a positive ticket; observe returns
// 0 pending, 1 complete or 2 failed; release cancels or releases a ticket.
#define BROOK_SYS_GPU_HANDOFF_CTL 0xB18
#define BROOK_GPU_HANDOFF_ATTACH 1
#define BROOK_GPU_HANDOFF_DEFAULT_DEPTH 2
#define BROOK_GPU_HANDOFF_MAX_DEPTH 3
#define BROOK_GPU_HANDOFF_RESERVE 2
#define BROOK_GPU_HANDOFF_OBSERVE 3
#define BROOK_GPU_HANDOFF_RELEASE 4
#define BROOK_GPU_HANDOFF_DETACH 5
// One-shot diagnostic: copy a completed protected output to a rectangle of
// the OS-owned display, sample its four corners, then request a normal desktop
// repaint. The kernel retains display ownership; this is not desktop takeover.
#define BROOK_GPU_HANDOFF_PRESENT_PROBE 6
// Experimental, full-screen tracked frame. Suppresses software copies for a
// short fixed lease; expiry or owner exit restores the software compositor.
// The renderer must supply the whole desktop; no scanout ownership is given.
#define BROOK_GPU_HANDOFF_PRESENT_LEASE 7
// Experimental region batch, relative to an active full-frame lease. The
// caller must include every changed pixel; this interface does not derive
// damage and is not yet suitable for a general desktop renderer.
#define BROOK_GPU_HANDOFF_PRESENT_REGIONS 8
// Cancel only this owner's queued submission when a dependency is pending.
// Active or ready work is never cancelled; a proven never-submitted output
// is released. A failed release remains quarantined.
#define BROOK_GPU_HANDOFF_CANCEL_BLOCKED 9
// Owner-bound cancellation plus admission freeze, frozen-work bitmask, and
// explicit abort. The freeze persists through exit until owner cleanup.
#define BROOK_GPU_HANDOFF_RECOVERY_BEGIN 10
#define BROOK_GPU_HANDOFF_RECOVERY_STATUS 11
#define BROOK_GPU_HANDOFF_RECOVERY_ABORT 12
// Exists only in BROOK_GPU_BLOCKED_RECOVERY_TEST kernels.
#define BROOK_GPU_HANDOFF_TEST_BLOCK_NEXT 13
// Full-frame presentation without sampling display pixels or writing a reply.
// Query supported operations once so newer renderers can still use older
// kernels with the four-corner PRESENT_LEASE interface.
#define BROOK_GPU_HANDOFF_PRESENT_CAPS 14
#define BROOK_GPU_HANDOFF_PRESENT_FRAME 15
#define BROOK_GPU_PRESENT_CAP_FRAME 1
// Queue one presentation from a completed render ticket. The result uses
// the original ticket. Zero is pending, one is success, and one plus errno
// is a retired presentation failure. Negative errors reject the lookup.
#define BROOK_GPU_HANDOFF_QUEUE_FRAME 16
#define BROOK_GPU_HANDOFF_QUEUE_REGIONS 17
#define BROOK_GPU_HANDOFF_PRESENT_RESULT 18
#define BROOK_GPU_PRESENT_CAP_ASYNC 2
// Opt-in partial reservation: argument is the output fd and the next syscall
// argument is the last accepted ticket used as the damage baseline. Rejected
// if the baseline changed before reservation. Existing RESERVE is unchanged.
#define BROOK_GPU_HANDOFF_RESERVE_DEPENDENT 19
#define BROOK_GPU_PRESENT_CAP_DEPENDENT 4
#define BROOK_GPU_RECOVERY_QUEUED 1
#define BROOK_GPU_RECOVERY_ACTIVE 2
#define BROOK_GPU_RECOVERY_QUARANTINED 4
#define BROOK_GPU_RECOVERY_UNKNOWN 8
#define BROOK_GPU_RECOVERY_RETIRING 16
#define BROOK_GPU_RECOVERY_PREPARING 32
#define BROOK_GPU_PRESENT_MAX_REGIONS 32
struct BrookGpuPresentRegion {
    uint64_t offset;
    uint32_t x,y,width,height,stride,reserved;
};
struct BrookGpuPresentRegions {
    uint64_t ticket;
    uint32_t count,reserved;
    struct BrookGpuPresentRegion regions[BROOK_GPU_PRESENT_MAX_REGIONS];
};
struct BrookGpuPresentProbe {
    uint64_t ticket;
    uint32_t x,y,width,height,stride,reserved;
    uint32_t samples[4];
};
struct BrookGpuPresentFrame {
    uint64_t ticket;
    uint32_t width,height,stride,reserved;
};

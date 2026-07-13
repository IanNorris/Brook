#pragma once

#include <stdint.h>

// Robust kernel stack unwinder for the panic / exception path.
//
// The legacy walks derefed the RBP chain blind (faulting the handler on a
// garbage-but-canonical RBP) and gave up the moment the chain broke (leaf
// frames, any code built without frame pointers, or a corrupt RBP truncated the
// trace to nothing). This unwinder:
//   * reads every frame through PanicSafe* so it can never nest a fault;
//   * validates each RBP against the thread's actual stack bounds (when known),
//     8-alignment, monotonicity, and return-address-in-.text;
//   * when the RBP chain breaks, falls back to a STACK SCAN — every qword in
//     [RSP, stackHi) that points into .text AND is immediately preceded by a
//     call instruction is emitted as a probable return address.
//
// Every frame carries a confidence tier so the human (and the host decoder) can
// tell a hardware-exact RIP from a heuristically-recovered one.

namespace brook {

enum PanicFrameConfidence : uint8_t {
    PANIC_FRAME_EXACT = 0,  // the interrupted RIP itself
    PANIC_FRAME_RBP   = 1,  // recovered via a validated RBP frame
    PANIC_FRAME_SCAN  = 2,  // heuristic: call-preceded .text ptr found on the stack
};

struct PanicUnwindFrame {
    uint64_t rip;
    uint8_t  confidence;   // PanicFrameConfidence
};

// Unwind up to maxFrames frames starting from (rip, rbp, rsp).
//   stackLo/stackHi: the thread's kernel stack bounds [lo, hi). Pass 0/0 if
//     unknown — a broad canonical kernel-half range is used instead (still safe
//     thanks to safe-reads + the .text/call-site checks).
// Returns the number of frames written to out.
uint32_t PanicUnwindStack(PanicUnwindFrame* out, uint32_t maxFrames,
                          uint64_t rip, uint64_t rbp, uint64_t rsp,
                          uint64_t stackLo, uint64_t stackHi);

}  // namespace brook

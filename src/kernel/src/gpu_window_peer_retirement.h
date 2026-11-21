#pragma once

#include <stdint.h>
#include "gpu_window_source_abi.h"

namespace brook {

bool GpuWindowPinBootProvider(uint64_t incarnation);
void GpuWindowRevokeBootProvider(uint64_t incarnation);

inline bool GpuWindowRestartProvider(uint64_t caller, uint64_t connector,
                                     uint64_t bootProvider, bool desktop, bool admin) {
    return caller && caller == connector && (admin || (desktop && caller == bootProvider));
}

// One receipt per renderer-to-provider queue. Socket callers serialize all
// access with the registry lock before checking the exact renderer's buffers.
struct GpuWindowPeerRetirement {
    enum class Result { Recorded, RestartClaimed, WrongRenderer };

    uint64_t incarnation;
    bool restartClaimed;
    bool recoveryWaitStarted;
    uint64_t recoveryWaitSinceMs;

    enum class Restart { Ready, NoOwner, Busy, AlreadySettled };

    Restart CheckRestart(uint64_t renderer, bool ownsOffscreen, bool recoveryPending, uint64_t nowMs) {
        if (!renderer || !ownsOffscreen) return Restart::NoOwner;
        if (incarnation || restartClaimed) return Restart::AlreadySettled;
        if (recoveryPending) {
            if (!recoveryWaitStarted) {
                recoveryWaitStarted = true;
                recoveryWaitSinceMs = nowMs;
            }
            if (nowMs - recoveryWaitSinceMs < BROOK_GPU_WINDOW_PEER_RECOVERY_GRACE_MS)
                return Restart::Busy;
        }
        return Restart::Ready;
    }

    bool Retired(uint64_t renderer) const {
        return renderer && incarnation == renderer;
    }

    Result Record(uint64_t renderer) {
        if (!renderer || (incarnation && incarnation != renderer)) return Result::WrongRenderer;
        if (restartClaimed) return Result::RestartClaimed;
        incarnation = renderer;
        return Result::Recorded;
    }

    bool ClaimRestart(uint64_t renderer) {
        if (!renderer || incarnation || restartClaimed) return false;
        restartClaimed = true;
        return true;
    }
};

}

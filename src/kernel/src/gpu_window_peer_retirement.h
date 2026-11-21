#pragma once

#include <stdint.h>

namespace brook {

// One receipt per renderer-to-provider queue. Socket callers serialize all
// access with the registry lock before checking the exact renderer's buffers.
struct GpuWindowPeerRetirement {
    enum class Result { Recorded, RestartClaimed, WrongRenderer };

    uint64_t incarnation;
    bool restartClaimed;

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

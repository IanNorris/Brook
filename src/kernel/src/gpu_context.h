#pragma once
#include <stdint.h>

namespace brook {
// Context identity travels with the DRM file and never follows the compositor.
struct GpuContextConfig {
    uint32_t capsetId = 1; // legacy/default VirGL
    uint32_t numRings = 0;
    uint64_t pollRingsMask = 0;
};
struct GpuContextParam { uint64_t param, value; };
struct GpuAppCapabilities {
    uint64_t capsetMask;
    bool contextInit;
    bool resourceBlob;
    bool hostVisible;
};
// Parse a copied parameter array transactionally. Unsupported debug names and
// poll-ring notifications stay unavailable until their semantics exist.
inline bool GpuContextParse(const GpuContextParam* params, uint32_t count,
                            const GpuAppCapabilities& caps,
                            GpuContextConfig& output) {
    if (!caps.contextInit || count > 4 || (count && !params)) return false;
    GpuContextConfig candidate{};
    uint32_t seen = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto p = params[i];
        if (p.param < 1 || p.param > 3) return false;
        const uint32_t bit = 1u << p.param;
        if (seen & bit) return false;
        seen |= bit;
        switch (p.param) {
        case 1:
            if (p.value >= 64 || !(caps.capsetMask & (uint64_t{1} << p.value)))
                return false;
            candidate.capsetId = static_cast<uint32_t>(p.value);
            break;
        case 2:
            if (p.value > 64) return false;
            candidate.numRings = static_cast<uint32_t>(p.value);
            break;
        case 3:
            if (p.value) return false;
            candidate.pollRingsMask = p.value;
            break;
        }
    }
    if (!(caps.capsetMask & (uint64_t{1} << candidate.capsetId))) return false;
    output = candidate;
    return true;
}
} // namespace brook

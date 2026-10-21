#pragma once
#include <stdint.h>

namespace brook {
// Opt-in fixed-HDA experiment. This is an engineering allowance for refill,
// not a scheduler guarantee or a bound on software-queue latency.
inline constexpr uint32_t AudioCommitHorizonFrames=4096;
inline constexpr uint32_t AudioRefillAllowanceMs=40;
inline constexpr uint32_t AudioRefillRate=44100;
inline constexpr uint32_t AudioRefillAllowanceFrames=
    (uint64_t(AudioRefillRate)*AudioRefillAllowanceMs+999)/1000;
inline constexpr uint64_t AudioRefillRequiredFrames(uint32_t fragmentFrames) {
    return uint64_t(AudioCommitHorizonFrames)+fragmentFrames+AudioRefillAllowanceFrames;
}
inline constexpr bool AudioRefillQualified(uint32_t capacityFrames,uint32_t fragmentFrames) {
    return fragmentFrames && fragmentFrames<=capacityFrames &&
           capacityFrames>=AudioRefillRequiredFrames(fragmentFrames);
}
} // namespace brook

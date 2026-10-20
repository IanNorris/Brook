#pragma once
#include <stdint.h>

namespace brook {

// A cyclic DMA position is not a lifetime counter. This state preserves the
// last proven completion watermark and refuses to guess after an ambiguous gap.
// Elapsed time only rejects samples; it never manufactures completed bytes.
class AudioDmaClock {
public:
    static constexpr uint32_t RingBytes = 65536;
    static constexpr uint32_t FrameBytes = 4;
    static constexpr uint32_t BytesPerSecond = 44100 * FrameBytes;
    static constexpr uint64_t RingDurationNs = uint64_t(RingBytes) * 1000000000ULL / BytesPerSecond;

    // Leave half a ring for codec prefetch/bursty emulator transfers. This is
    // a conservative sampling policy, not a formal maximum-rate certification.
    static constexpr uint64_t MaxSampleNs = RingDurationNs / 2;

    uint64_t epoch = 1;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    bool running = false;
    bool discontinuity = false;

    bool Accept(uint32_t bytes) {
        if (discontinuity || bytes % FrameBytes || submitted < completed || bytes > RingBytes - Pending() ||
            submitted > ~uint64_t(0) - bytes) return false;
        submitted += bytes;
        return true;
    }
    uint32_t Pending() const { return submitted > completed ? static_cast<uint32_t>(submitted - completed) : 0; }
    // Zero-filled idle intervals have no application owner. Reserve a safe
    // future physical position without inventing completion of any receipt.
    bool ReserveStart(uint64_t begin) {
        return !Pending() && ReserveGap(begin);
    }
    // Preserve any earlier owned receipts while reserving an unowned silent
    // interval before the next one. Completion of that gap must never be
    // counted as application frames; callers reconcile by receipt begin/end.
    bool ReserveGap(uint64_t begin) {
        if (discontinuity || begin < submitted || begin < completed ||
            begin % FrameBytes || begin - completed >= RingBytes) return false;
        submitted = begin;
        return true;
    }
    uint64_t RawCompleted() const { return rawCompleted; }
    uint64_t SampleBeforeNs() const { return lastBeforeNs; }
    uint32_t Position() const { return lastPosition; }
    void Fault() { discontinuity = true; running = false; }
    bool Start(uint32_t position, uint64_t beforeNs) {
        if (discontinuity || !beforeNs || position >= RingBytes) { Fault(); return false; }
        lastPosition = position;
        lastBeforeNs = beforeNs;
        running = true;
        return true;
    }
    bool Observe(uint32_t position, uint64_t beforeNs, uint64_t afterNs, bool deviceError) {
        if (discontinuity) return false;
        if (!running) return true;
        if (deviceError || position >= RingBytes || !beforeNs || beforeNs < lastBeforeNs ||
            afterNs < beforeNs || afterNs - lastBeforeNs >= MaxSampleNs) {
            Fault(); return false;
        }
        const uint32_t delta = (position + RingBytes - lastPosition) % RingBytes;
        // Retain sub-frame position movement; expose only whole-frame completion.
        if (rawCompleted > ~uint64_t(0) - delta) { Fault(); return false; }
        rawCompleted += delta;
        completed = rawCompleted - rawCompleted % FrameBytes;
        lastPosition = position;
        lastBeforeNs = beforeNs;
        return true;
    }
    // Stopping a fully drained physical ring must not erase logical completion.
    bool Drained() {
        if (discontinuity || Pending()) return false;
        running = false;
        submitted = rawCompleted = completed;
        return true;
    }
    // Acknowledge only the observed epoch; never erase an unknown newer fault.
    bool Restart(uint64_t expectedEpoch) {
        if (!discontinuity || expectedEpoch != epoch || epoch == ~uint64_t(0)) return false;
        ++epoch;
        submitted = completed = rawCompleted = lastBeforeNs = 0;
        lastPosition = 0;
        running = discontinuity = false;
        return true;
    }
private:
    uint64_t rawCompleted = 0;
    uint64_t lastBeforeNs = 0;
    uint32_t lastPosition = 0;
};

} // namespace brook

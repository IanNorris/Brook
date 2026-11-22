#pragma once
#include <stdint.h>

// USB Audio Class 2 playback helpers for the xHCI driver: descriptor parsing,
// async feedback pacing, isochronous TRB words and a fixed-point test tone.
// Pure functions only, so the host tests cover them.
namespace brook {

struct Uac2Playback {
    bool found = false;
    uint8_t controlInterface = 0;   // AudioControl interface number
    uint8_t clockId = 0;            // clock source feeding the playback terminal
    uint8_t featureId = 0;          // feature unit fed by the USB streaming terminal
    uint8_t featureChannels = 0;    // logical channels of that feature unit
    uint32_t featureVolumeMask = 0; // bit n: channel n (0 = master) has a volume control
    uint8_t terminalId = 0;         // USB streaming input terminal
    uint8_t streamInterface = 0;
    uint8_t alternate = 0;
    uint8_t channels = 0;
    uint8_t subslot = 0;            // bytes per sample
    uint8_t bits = 0;
    uint8_t dataEp = 0;             // isochronous OUT
    uint16_t dataMaxPacket = 0;
    uint8_t dataInterval = 0;
    uint8_t feedbackEp = 0;         // isochronous IN, explicit feedback
    uint16_t feedbackMaxPacket = 0;
    uint8_t feedbackInterval = 0;
};

// Find the playback streaming alternate with the wanted sample width (16 or
// 24 bits), its endpoints, and the clock and feature unit that serve it.
// Interfaces whose class is not Audio (for example DFU) are never selected.
inline bool Uac2ParsePlayback(const uint8_t* d, uint32_t length, uint8_t wantBits, Uac2Playback& out)
{
    out = {};
    struct Terminal { uint8_t id, clock; uint16_t type; };
    Terminal terminals[16]{}; unsigned terminalCount = 0;
    struct Feature { uint8_t id, source, channels; uint32_t volume; };
    Feature features[16]{}; unsigned featureCount = 0;
    struct Clock { uint8_t id, input; bool source; };
    Clock clocks[16]{}; unsigned clockCount = 0;
    uint8_t ifNum = 0, ifAlt = 0, ifClass = 0, ifSub = 0;
    // Candidate alternate being collected.
    Uac2Playback cand{}; bool candOk = false;
    auto finish = [&]() {
        if (candOk && cand.dataEp && cand.bits == wantBits && !out.found) {
            out = cand; out.found = true;
        }
        candOk = false; cand = {};
    };
    for (uint32_t o = 0; o + 2 <= length;) {
        const uint8_t len = d[o], type = d[o + 1];
        if (len < 2 || o + len > length) break;
        const uint8_t* p = d + o;
        if (type == 4 && len >= 9) {
            finish();
            ifNum = p[2]; ifAlt = p[3]; ifClass = p[5]; ifSub = p[6];
            if (ifClass == 1 && ifSub == 1) { out.controlInterface = ifNum; cand.controlInterface = ifNum; }
            if (ifClass == 1 && ifSub == 2 && ifAlt) {
                candOk = true; cand.streamInterface = ifNum; cand.alternate = ifAlt;
            }
        } else if (type == 0x24 && ifClass == 1 && ifSub == 1 && len >= 3) {
            const uint8_t sub = p[2];
            if (sub == 2 && len >= 17 && terminalCount < 16)       // input terminal
                terminals[terminalCount++] = {p[3], p[7], uint16_t(p[4] | p[5] << 8)};
            else if (sub == 0x0a && len >= 8 && clockCount < 16)    // clock source
                clocks[clockCount++] = {p[3], 0, true};
            else if (sub == 0x0b && len >= 7 && p[4] && clockCount < 16) // clock selector: first input
                clocks[clockCount++] = {p[3], p[5], false};
            else if (sub == 6 && len >= 6 && featureCount < 16) {   // feature unit
                const uint8_t n = uint8_t((len - 6) / 4);
                uint32_t mask = 0;
                for (uint8_t c = 0; c < n && c < 32; ++c) {
                    const uint32_t controls = uint32_t(p[5 + 4 * c]) | uint32_t(p[6 + 4 * c]) << 8;
                    if (controls & 0x0c) mask |= 1u << c; // volume control present
                }
                features[featureCount++] = {p[3], p[4], uint8_t(n ? n - 1 : 0), mask};
            }
        } else if (type == 0x24 && candOk && len >= 3) {
            if (p[2] == 1 && len >= 16) { cand.terminalId = p[3]; cand.channels = p[10]; }
            else if (p[2] == 2 && len >= 6) { cand.subslot = p[4]; cand.bits = p[5]; }
        } else if (type == 5 && len >= 7 && candOk) {
            const uint8_t attr = p[3];
            const uint16_t mp = uint16_t(p[4] | p[5] << 8);
            if ((attr & 3) == 1) {
                const bool in = p[2] & 0x80, feedback = ((attr >> 4) & 3) == 1;
                if (!in && !feedback) { cand.dataEp = p[2]; cand.dataMaxPacket = mp; cand.dataInterval = p[6]; }
                else if (in && feedback) { cand.feedbackEp = p[2]; cand.feedbackMaxPacket = mp; cand.feedbackInterval = p[6]; }
            }
        }
        o += len;
    }
    finish();
    if (!out.found) return false;
    out.found = false;
    for (unsigned i = 0; i < terminalCount; ++i)
        if (terminals[i].id == out.terminalId && terminals[i].type == 0x0101) out.clockId = terminals[i].clock;
    // Follow clock selectors (by their first input) to the clock source.
    for (unsigned hop = 0; hop < 4 && out.clockId; ++hop) {
        bool source = false, moved = false;
        for (unsigned i = 0; i < clockCount; ++i)
            if (clocks[i].id == out.clockId) {
                if (clocks[i].source) source = true; else { out.clockId = clocks[i].input; moved = true; }
                break;
            }
        if (source) break;
        if (!moved) { out.clockId = 0; break; }
    }
    for (unsigned i = 0; i < featureCount; ++i)
        if (features[i].source == out.terminalId) {
            out.featureId = features[i].id; out.featureChannels = features[i].channels;
            out.featureVolumeMask = features[i].volume;
        }
    out.found = out.clockId && out.dataEp && out.feedbackEp && out.subslot && out.channels &&
                out.dataMaxPacket && (out.dataMaxPacket & 0x7ff) >= out.subslot * out.channels;
    return out.found;
}

// Explicit feedback for high speed: frames per microframe in 16.16. Devices
// differ in how they justify it, so (as Linux does) accept the first shift
// that lands within +-1/8 of nominal; otherwise keep the previous value.
struct Uac2FeedbackPacer {
    uint32_t nominal = 0;      // 16.16 frames per service interval
    uint32_t current = 0;
    uint32_t accumulator = 0;
    uint32_t maxFrames = 0;
    uint32_t accepted = 0, rejected = 0;
    uint32_t rate = 0, intervals = 0, exact = 0; // exact nominal pacing until feedback arrives
    void Start(uint32_t sampleRate, uint32_t intervalsPerSecond, uint32_t maxFramesPerPacket) {
        rate = sampleRate; intervals = intervalsPerSecond; exact = 0;
        nominal = uint32_t((uint64_t(rate) << 16) / intervalsPerSecond);
        current = nominal; accumulator = 0; maxFrames = maxFramesPerPacket; accepted = rejected = 0;
    }
    bool Feed(uint32_t raw, uint32_t bytes) {
        if (bytes == 3) raw = (raw & 0xffffff) << 2;   // full-speed 10.14 in 3 bytes
        const uint64_t lo = nominal - nominal / 8, hi = nominal + nominal / 8;
        const uint64_t candidates[4] = {raw, uint64_t(raw) << 2, raw >> 4, uint64_t(raw) << 4};
        for (uint64_t value : candidates)
            if (value >= lo && value <= hi) { current = uint32_t(value); ++accepted; return true; }
        ++rejected; return false;
    }
    // Frames for the next packet; never more than maxFrames, never zero
    // unless the nominal rate is below one frame per interval.
    uint32_t Next() {
        if (!accepted && intervals) {
            exact += rate;
            uint32_t frames = exact / intervals;
            exact %= intervals;
            return frames > maxFrames ? maxFrames : frames;
        }
        accumulator += current;
        uint32_t frames = accumulator >> 16;
        accumulator &= 0xffffu;
        if (frames > maxFrames) frames = maxFrames;
        return frames;
    }
};

// Isochronous TRB words (xHCI 1.x 6.4.1.3) for a single-packet high-speed TD.
constexpr uint32_t XhciIsochType = 5u << 10;
constexpr uint32_t XhciIsochStatus(uint32_t bytes) { return bytes & 0x1ffffu; } // TD size 0, interrupter 0
constexpr uint32_t XhciIsochControl(bool cycle, bool ioc, bool sia, uint32_t frameId = 0)
{
    return (cycle ? 1u : 0u) | (1u << 2) /*ISP*/ | (ioc ? 1u << 5 : 0u) | XhciIsochType |
           (sia ? 1u << 31 : (frameId & 0x7ffu) << 20);
}

// Endpoint context dwords 0, 1 and 4 for an isochronous endpoint (6.2.3).
struct XhciIsochEndpoint { uint32_t dw0, dw1, dw4; };
inline XhciIsochEndpoint XhciIsochContext(bool in, uint8_t bInterval, uint16_t wMaxPacket, uint32_t averageBytes)
{
    const uint32_t packet = wMaxPacket & 0x7ffu, mult = (wMaxPacket >> 11) & 3u;
    const uint32_t interval = bInterval ? uint32_t(bInterval - 1) : 0u; // HS: 2^(bInterval-1) microframes
    const uint32_t esit = packet * (mult + 1);
    return {interval << 16, (in ? 5u : 1u) << 3 | (mult << 8) | (packet << 16), (averageBytes & 0xffffu) | (esit << 16)};
}

// UAC2 volume in 1/256 dB steps, clamped to the device range.
inline int16_t Uac2VolumeQ8(int dB, int16_t minimum, int16_t maximum)
{
    int32_t v = int32_t(dB) * 256;
    if (v < minimum) v = minimum;
    if (v > maximum) v = maximum;
    return int16_t(v);
}

// Sine in Q15 from a 32-bit phase (one cycle = 2^32), Bhaskara I
// approximation: |error| < 0.2% of full scale, no floating point.
inline int32_t Uac2SineQ15(uint32_t phase)
{
    const bool negative = phase & 0x80000000u;
    const uint64_t t = (phase & 0x7fffffffu) >> 15;            // 0..65535 over a half cycle
    const uint64_t u = (t * (65536 - t)) >> 16;                 // t(1-t), Q16
    const int32_t value = int32_t((16 * u * 32767) / (5 * 65536 - 4 * u));
    return negative ? -value : value;
}

struct Uac2Tone {
    uint32_t phase = 0, step = 0, remaining = 0, total = 0, fade = 0;
    int32_t amplitude = 0;      // Q15 peak
    void Start(uint32_t hz, uint32_t rate, uint32_t frames, uint32_t fadeFrames, int32_t peakQ15) {
        phase = 0; step = uint32_t((uint64_t(hz) << 32) / rate); remaining = total = frames;
        fade = fadeFrames; amplitude = peakQ15;
    }
    bool Active() const { return remaining != 0; }
    int16_t NextSample() {
        if (!remaining) return 0;
        const uint32_t played = total - remaining;
        int64_t gain = amplitude;
        if (fade && played < fade) gain = gain * played / fade;
        if (fade && remaining <= fade) gain = gain * (remaining - 1) / fade;
        const int32_t s = int32_t((int64_t(Uac2SineQ15(phase)) * gain) >> 15);
        phase += step; --remaining;
        return int16_t(s);
    }
};

}

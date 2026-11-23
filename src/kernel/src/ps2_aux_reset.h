#pragma once
// PS/2 auxiliary (mouse/touchpad) reset and self-test wait, shared by
// mouse.cpp and the host test (test_ps2_aux_reset). Io provides:
//   uint8_t In(uint16_t port); void Out(uint16_t port, uint8_t value);
//   uint64_t NowMs(); void Pause();
//
// Waits are bounded by elapsed time. An iteration cap (ms * kReadsPerMs) only
// matters when the clock does not advance; it must never be the binding limit
// on real hardware, where one status read costs about 1 us on an i8042/EC and
// several us under SMM 8042 emulation.

#include <stdint.h>

namespace brook {

enum class Ps2AuxReset : uint8_t { Ok, NoDevice, SelfTestFailed, Timeout };

struct Ps2AuxResetOutcome {
    Ps2AuxReset result;
    uint8_t selfTest;  // 0xAA ok, 0xFC failed, 0 none
    uint8_t deviceId;
};

// A device ACKs a command within 25 ms; no byte at all after this means no
// auxiliary device is present.
constexpr uint64_t kPs2AuxAckMs = 200;
// Basic assurance test after reset: Linux libps2 allows 4 s for
// PS2_CMD_RESET_BAT. Touchpads can take well over 1 s.
constexpr uint64_t kPs2AuxBatMs = 4000;
constexpr uint64_t kPs2ReadsPerMs = 2000;

template <typename Io>
bool Ps2WaitStatus(Io& io, uint8_t mask, bool set, uint64_t ms)
{
    const uint64_t start = io.NowMs();
    const uint64_t cap = (ms ? ms : 1) * kPs2ReadsPerMs;
    for (uint64_t i = 0; i < cap; ++i) {
        const bool on = (io.In(0x64) & mask) != 0;
        if (on == set) return true;
        if ((i & 63) == 63 && io.NowMs() - start >= ms) return false;
        io.Pause();
    }
    return false;
}

template <typename Io> bool Ps2WaitWrite(Io& io, uint64_t ms = 50) { return Ps2WaitStatus(io, 0x02, false, ms); }
template <typename Io> bool Ps2WaitRead(Io& io, uint64_t ms) { return Ps2WaitStatus(io, 0x01, true, ms); }

// Send 0xFF to the auxiliary device and wait for ACK, BAT result and ID.
template <typename Io>
Ps2AuxResetOutcome Ps2AuxResetDevice(Io& io)
{
    Ps2WaitWrite(io);
    io.Out(0x64, 0xD4);
    Ps2WaitWrite(io);
    io.Out(0x60, 0xFF);
    if (!Ps2WaitRead(io, kPs2AuxAckMs)) return {Ps2AuxReset::NoDevice, 0, 0};
    uint8_t first = io.In(0x60);
    const uint64_t start = io.NowMs();
    // Normally 0xFA then 0xAA 0x00. Skip ACK/resend bytes; a device that
    // answers 0xAA directly (no ACK) is accepted too.
    for (;;) {
        if (first == 0xAA || first == 0xFC) break;
        const uint64_t spent = io.NowMs() - start;
        if (spent >= kPs2AuxBatMs || !Ps2WaitRead(io, kPs2AuxBatMs - spent))
            return {Ps2AuxReset::Timeout, 0, 0};
        first = io.In(0x60);
    }
    if (first == 0xFC) return {Ps2AuxReset::SelfTestFailed, 0xFC, 0};
    uint8_t id = 0;
    if (Ps2WaitRead(io, 100)) id = io.In(0x60);
    return {Ps2AuxReset::Ok, 0xAA, id};
}

// After a failed auxiliary init: disable the port, mask its interrupt and
// clock in the controller config byte, and drain the output buffer, so late
// bytes from the device cannot occupy the shared 8042 output buffer and block
// the keyboard.
template <typename Io>
void Ps2AuxDisable(Io& io)
{
    Ps2WaitWrite(io);
    io.Out(0x64, 0xA7);
    for (unsigned i = 0; i < 32 && (io.In(0x64) & 1); ++i) (void)io.In(0x60);
    Ps2WaitWrite(io);
    io.Out(0x64, 0x20);
    if (Ps2WaitRead(io, 100)) {
        uint8_t cfg = io.In(0x60);
        cfg = static_cast<uint8_t>((cfg & ~0x02) | 0x20);
        Ps2WaitWrite(io);
        io.Out(0x64, 0x60);
        Ps2WaitWrite(io);
        io.Out(0x60, cfg);
    }
    for (unsigned i = 0; i < 32 && (io.In(0x64) & 1); ++i) (void)io.In(0x60);
}

} // namespace brook

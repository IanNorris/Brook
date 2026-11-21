#pragma once
#include <stdint.h>

namespace brook {
namespace vmmouse {
constexpr uint32_t Magic = 0x564d5868u;
constexpr uint32_t Version = 0x3442554au;
constexpr uint32_t GetVersion = 10;
constexpr uint32_t Data = 39;
constexpr uint32_t Status = 40;
constexpr uint32_t Command = 41;
constexpr uint32_t Restrict = 86;
constexpr uint32_t Enable = 0x45414552u;
constexpr uint32_t Disable = 0xf5;
constexpr uint32_t Absolute = 0x53424152u;
constexpr uint32_t Relative = 0x4c455252u;
constexpr unsigned DrainLimit = 256;

struct Reply {
    uint32_t a, b, c, d;
};

inline bool Failed(uint32_t status)
{
    return (status & 0xffff0000u) == 0xffff0000u;
}

template<class Call>
bool Start(Call call)
{
    auto probe = call(GetVersion, 0);
    if (probe.b != Magic || probe.a == 0xffffffffu) return false;

    // Reset any existing device mode before reading its protocol identifier.
    call(Command, Disable);
    call(Command, Enable);
    auto status = call(Status, 0).a;
    if (!Failed(status) && (status & 0xffffu) != 0 && call(Data, 1).a == Version) {
        call(Restrict, 1); // Optional CPL0 restriction on VMware.
        call(Command, Absolute);
        if (!Failed(call(Status, 0).a)) return true;
    }
    call(Command, Disable);
    return false;
}

struct Packet {
    uint32_t x, y;
    uint8_t buttons;
    int32_t wheel;
    bool relative;
};

inline int32_t Pixel(uint32_t absolute, uint32_t extent)
{
    if (extent < 2) return 0;
    if (absolute > 0xffffu) absolute = 0xffffu;
    return static_cast<int32_t>(static_cast<uint64_t>(absolute) * (extent - 1) / 0xffffu);
}

template<class Call, class Emit>
bool Drain(Call call, Emit emit)
{
    // IRQ12 signals availability. Bound the work even if the host continues
    // appending samples, and leave subsequent notification bytes to drain more.
    for (unsigned i = 0; i < DrainLimit; ++i) {
        const auto status = call(Status, 0).a;
        const auto words = status & 0xffffu;
        if (Failed(status) || words % 4) return false;
        if (!words) return true;
        const auto r = call(Data, 4);
        if (Failed(r.a) || r.a == Magic) return false;
        const uint8_t buttons = ((r.a & 0x20) >> 5) | ((r.a & 0x10) >> 3) | ((r.a & 0x08) >> 1);
        emit(Packet{r.b, r.c, buttons, -int32_t(static_cast<int8_t>(r.d)),
                    (r.a & 0x00010000u) != 0});
    }
    return true;
}
}
}

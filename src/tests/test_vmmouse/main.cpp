#include <cassert>
#include <deque>
#include <vector>
#include "../../kernel/src/vmmouse.h"

using namespace brook::vmmouse;

struct Device {
    bool present = true;
    bool enabled = false;
    bool absolute = false;
    uint32_t version = Version;
    std::deque<Reply> packets;
    uint32_t statusOverride = 0;
    unsigned calls = 0;

    Reply Call(uint32_t op, uint32_t arg)
    {
        ++calls;
        switch (op) {
        case GetVersion: return {present ? 1u : UINT32_MAX, present ? Magic : 0u, 0, 0};
        case Command:
            if (arg == Enable) enabled = true;
            if (arg == Disable) enabled = false;
            if (arg == Absolute) absolute = true;
            return {};
        case Status:
            if (statusOverride) return {statusOverride, 0, 0, 0};
            return {enabled ? (absolute ? uint32_t(packets.size() * 4) : 1u) : 0xffff0000u, 0, 0, 0};
        case Data:
            if (arg == 1) return {version, 0, 0, 0};
            assert(arg == 4 && !packets.empty());
            {
                auto reply = packets.front();
                packets.pop_front();
                return reply;
            }
        case Restrict:
            assert(arg == 1);
            return {};
        }
        assert(false);
        return {};
    }
};

int main()
{
    Device device;
    auto call = [&](uint32_t op, uint32_t arg) { return device.Call(op, arg); };
    device.present = false;
    assert(!Start(call) && device.calls == 1 && !device.enabled);
    device.present = true;
    device.version = 0;
    assert(!Start(call) && !device.enabled && !device.absolute);
    device.version = Version;
    assert(Start(call) && device.enabled && device.absolute);

    assert(Pixel(0, 3200) == 0);
    assert(Pixel(65535, 3200) == 3199);
    assert(Pixel(65536, 1800) == 1799);
    assert(Pixel(32768, 1920) == 959);
    assert(Pixel(42, 1) == 0);
    assert(RelativeDelta(0xfffffff9u) == -7);
    assert(RelativeDelta(0xfffffff9u, true) == 7);
    assert(RelativeDelta(0x80000000u, true) == 32767);

    device.packets.push_back({0x20 | 0x10, 0, 65535, 0xff});
    device.packets.push_back({0x10000 | 0x08, 3, 0xfffffffcu, 1});
    std::vector<Packet> samples;
    auto emit = [&](Packet packet) { samples.push_back(packet); };
    assert(Drain(call, emit) && samples.size() == 2);
    assert(!samples[0].relative && samples[0].buttons == 3 && samples[0].wheel == 1);
    assert(samples[0].x == 0 && samples[0].y == 65535);
    assert(samples[1].relative && samples[1].buttons == 4 && samples[1].wheel == -1);
    assert(static_cast<int32_t>(samples[1].y) == -4);
    device.statusOverride = 0xffff0000;
    assert(!Drain(call, emit));
    device.statusOverride = 3;
    assert(!Drain(call, emit));
    device.statusOverride = 0;
    device.packets.push_back({Magic, 0, 0, 0});
    assert(!Drain(call, emit) && samples.size() == 2);

    device.packets.resize(DrainLimit + 1);
    assert(Drain(call, emit) && device.packets.size() == 1);
    assert(Drain(call, emit) && device.packets.empty());
}

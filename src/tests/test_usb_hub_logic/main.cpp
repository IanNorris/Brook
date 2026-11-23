#include "usb_hub_logic.h"
#include <cassert>
#include <cstdio>
using namespace brook;

static void Topology()
{
    // Root-port device: no route, no TT.
    const auto root = UsbRootTopology(3, UsbSpeedHigh);
    assert(root.route == 0 && root.depth == 0 && root.rootPort == 3 && !root.ttSlot);

    // Full-speed keyboard on port 2 of a high-speed hub (slot 5) on root port 3.
    UsbTopology kbd;
    assert(UsbChildTopology(root, 5, false, 2, UsbSpeedFull, kbd));
    assert(kbd.route == 0x2 && kbd.depth == 1 && kbd.rootPort == 3);
    assert(kbd.ttSlot == 5 && kbd.ttPort == 2 && !kbd.ttMulti);

    // High-speed NIC behind the same hub needs no TT.
    UsbTopology nic;
    assert(UsbChildTopology(root, 5, false, 4, UsbSpeedHigh, nic));
    assert(nic.route == 0x4 && !nic.ttSlot);

    // Nested: HS hub (slot 6) on port 1 of the first hub, low-speed mouse on
    // its port 3: TT comes from the nearest HS hub (slot 6, port 3).
    UsbTopology hub2;
    assert(UsbChildTopology(root, 5, false, 1, UsbSpeedHigh, hub2));
    UsbTopology mouse;
    assert(UsbChildTopology(hub2, 6, true, 3, UsbSpeedLow, mouse));
    assert(mouse.route == 0x31 && mouse.depth == 2 && mouse.ttSlot == 6 && mouse.ttPort == 3 && mouse.ttMulti);

    // A full-speed hub behind an HS hub passes the HS hub's TT down.
    UsbTopology fsHub, fsDev;
    assert(UsbChildTopology(root, 5, false, 2, UsbSpeedFull, fsHub));
    assert(UsbChildTopology(fsHub, 9, false, 1, UsbSpeedFull, fsDev));
    assert(fsDev.ttSlot == 5 && fsDev.ttPort == 2 && fsDev.route == 0x12);

    // A full-speed hub on a root port (QEMU usb-hub): root TT, no fields.
    const auto qemuHub = UsbRootTopology(1, UsbSpeedFull);
    UsbTopology qemuKbd;
    assert(UsbChildTopology(qemuHub, 1, false, 2, UsbSpeedFull, qemuKbd));
    assert(!qemuKbd.ttSlot && qemuKbd.route == 0x2);

    // SuperSpeed hub chain: no TT, ports above 15 clamp to 15.
    UsbTopology ss = UsbRootTopology(17, UsbSpeedSuper), next;
    for (uint8_t depth = 0; depth < UsbMaxRouteDepth; ++depth) {
        assert(UsbChildTopology(ss, 1, false, depth == 4 ? 20 : depth + 1, UsbSpeedSuper, next));
        ss = next;
    }
    assert(ss.depth == 5 && ss.route == 0xF4321 && ss.rootPort == 17 && !ss.ttSlot);
    assert(!UsbChildTopology(ss, 1, false, 1, UsbSpeedSuper, next)); // sixth tier refused
    assert(!UsbChildTopology(root, 5, false, 0, UsbSpeedFull, next));  // port 0 invalid
}

static void SlotContexts()
{
    const auto root = UsbRootTopology(3, UsbSpeedHigh);
    // HS hub with 4 ports, TT think time 2 -> Hub bit, ports, TTT.
    auto w = UsbSlotContext(root, 1, true, 4, 2, false);
    assert(w.dw0 == ((1u << 27) | (1u << 26) | (3u << 20)));
    assert(w.dw1 == ((3u << 16) | (4u << 24)));
    assert(w.dw2 == (2u << 16));
    // Multi-TT HS hub sets MTT.
    w = UsbSlotContext(root, 1, true, 7, 0, true);
    assert(w.dw0 & (1u << 25));
    // FS device behind it: TT slot/port, MTT inherited, no hub fields.
    UsbTopology kbd;
    assert(UsbChildTopology(root, 5, true, 2, UsbSpeedFull, kbd));
    w = UsbSlotContext(kbd, 1, false, 0, 0, false);
    assert(w.dw0 == ((1u << 27) | (1u << 25) | (1u << 20) | 0x2));
    assert(w.dw1 == (3u << 16));
    assert(w.dw2 == (5u | (2u << 8)));
    // SuperSpeed hub: no TTT, no MTT.
    const auto ssHub = UsbRootTopology(18, UsbSpeedSuper);
    w = UsbSlotContext(ssHub, 1, true, 4, 3, true);
    assert(w.dw0 == ((1u << 27) | (1u << 26) | (4u << 20)) && w.dw2 == 0);
}

static void PortStatus()
{
    // USB2 hub: connected, enabled, powered, high-speed.
    auto s = UsbHubDecodePort(0x0503, false);
    assert(s.connected && s.enabled && s.powered && s.speed == UsbSpeedHigh);
    s = UsbHubDecodePort(0x0303, false);
    assert(s.speed == UsbSpeedLow);
    s = UsbHubDecodePort(0x0101, false);
    assert(s.connected && !s.enabled && s.speed == UsbSpeedFull);
    // SuperSpeed hub: power is bit 9, link state bits 8:5 (U0 = 0, Inactive = 6).
    s = UsbHubDecodePort(0x0203, true);
    assert(s.connected && s.enabled && s.powered && s.linkState == 0 && s.speed == UsbSpeedSuper);
    s = UsbHubDecodePort(0x02C1, true);
    assert(s.linkState == 6 && !s.enabled);

    uint16_t f[8];
    assert(UsbHubChangeFeatures(0x0011, false, f, 8) == 2 && f[0] == UsbHubFeatCConnection && f[1] == UsbHubFeatCReset);
    assert(UsbHubChangeFeatures(0x0002, false, f, 8) == 1 && f[0] == UsbHubFeatCEnable);
    // SS hubs have no C_PORT_ENABLE; bit 5 is the BH reset change.
    assert(UsbHubChangeFeatures(0x0022, true, f, 8) == 1 && f[0] == UsbHubFeatCBhReset);
    assert(UsbHubChangeFeatures(0x00C0, true, f, 8) == 2 && f[0] == UsbHubFeatCLinkState && f[1] == UsbHubFeatCConfigError);
}

static void Intervals()
{
    uint32_t i = 0;
    assert(UsbXhciInterval(UsbSpeedFull, 10, i) && i == 6);   // 8 frames = 64 microframes
    assert(UsbXhciInterval(UsbSpeedLow, 1, i) && i == 3);
    assert(UsbXhciInterval(UsbSpeedFull, 255, i) && i == 10);
    assert(UsbXhciInterval(UsbSpeedHigh, 4, i) && i == 3);
    assert(UsbXhciInterval(UsbSpeedSuper, 1, i) && i == 0);
    assert(!UsbXhciInterval(UsbSpeedHigh, 17, i));
    assert(!UsbXhciInterval(UsbSpeedFull, 0, i));

    assert(UsbDefaultEp0Packet(UsbSpeedLow) == 8 && UsbDefaultEp0Packet(UsbSpeedFull) == 64);
    assert(UsbDefaultEp0Packet(UsbSpeedSuper) == 512 && UsbDefaultEp0Packet(UsbSpeedSuperPlus) == 512);
    assert(UsbEp0PacketFromDescriptor(UsbSpeedFull, 8) == 8);
    assert(UsbEp0PacketFromDescriptor(UsbSpeedFull, 12) == 0);
    assert(UsbEp0PacketFromDescriptor(UsbSpeedSuper, 9) == 512);
}

static void RootActions()
{
    constexpr uint32_t ccs = 1, ped = 2, pp = 1u << 9;
    auto pls = [](uint32_t s) { return s << 5; };
    assert(XhciRootPortAction(pp | pls(5), true) == XhciRootAction::Idle);          // empty SS port
    assert(XhciRootPortAction(pp | ccs | ped | (4u << 10), true) == XhciRootAction::Enumerate);
    assert(XhciRootPortAction(pp | ccs | pls(7), true) == XhciRootAction::Wait);    // Polling
    assert(XhciRootPortAction(pp | pls(6), true) == XhciRootAction::WarmReset);     // SS.Inactive
    assert(XhciRootPortAction(pp | ccs | pls(10), true) == XhciRootAction::WarmReset); // Compliance
    assert(XhciRootPortAction(pp | ccs | pls(4), true) == XhciRootAction::WarmReset);
    assert(XhciRootPortAction(pp | ccs | pls(7), false) == XhciRootAction::Reset);  // USB2 needs reset
    assert(XhciRootPortAction(pp | ccs | ped, false) == XhciRootAction::Enumerate);
    assert(XhciRootPortAction(pp | ccs | (1u << 4), false) == XhciRootAction::Wait); // reset in progress
    assert(XhciRootPortAction(pp, false) == XhciRootAction::Idle);
    assert(XhciRootPortAction(0xffffffffu, false) == XhciRootAction::Idle);
}

int main()
{
    Topology();
    SlotContexts();
    PortStatus();
    Intervals();
    RootActions();
    std::puts("usb_hub_logic: all tests passed");
    return 0;
}

#pragma once
#include <stdint.h>

// Pure USB topology helpers shared by the xHCI driver and host tests:
// route strings, transaction-translator selection, hub port status decode,
// slot contexts and root-port actions. No MMIO, no allocation.
namespace brook {

// xHCI default Protocol Speed IDs (xHCI 1.2 §7.2.2.1.1).
enum : uint32_t { UsbSpeedFull = 1, UsbSpeedLow = 2, UsbSpeedHigh = 3,
                  UsbSpeedSuper = 4, UsbSpeedSuperPlus = 5 };

constexpr bool UsbSpeedIsSuper(uint32_t speed) { return speed >= UsbSpeedSuper; }
constexpr bool UsbSpeedNeedsTt(uint32_t speed) { return speed == UsbSpeedFull || speed == UsbSpeedLow; }

// Maximum hub tiers between the root port and a device (USB 3.2 §10.1.3;
// the xHCI route string carries five 4-bit port numbers).
constexpr uint8_t UsbMaxRouteDepth = 5;

// Where a device sits: route string (tier 1 in bits 3:0), root port, number
// of hubs above it, its speed and the nearest high-speed hub providing a TT.
struct UsbTopology {
    uint32_t route = 0;
    uint8_t depth = 0;       // 0 = attached to a root port
    uint8_t rootPort = 0;    // 1-based root hub port
    uint32_t speed = 0;
    uint8_t ttSlot = 0;      // TT hub slot (0 = none, root port TT)
    uint8_t ttPort = 0;      // port on the TT hub on this device's path
    bool ttMulti = false;    // the TT hub runs in multi-TT mode
};

constexpr UsbTopology UsbRootTopology(uint8_t rootPort, uint32_t speed) {
    UsbTopology t{};
    t.rootPort = rootPort; t.speed = speed;
    return t;
}

// Topology of a device on 'port' (1-based) of a hub. Returns false when the
// hub is already at the maximum depth or the port number is invalid.
inline bool UsbChildTopology(const UsbTopology& hub, uint8_t hubSlot, bool hubMulti,
                             uint8_t port, uint32_t childSpeed, UsbTopology& out) {
    if (hub.depth >= UsbMaxRouteDepth || !port) return false;
    out = {};
    const uint32_t nibble = port > 15 ? 15u : port;
    out.route = (hub.route & ((1u << (4 * hub.depth)) - 1u)) | (nibble << (4 * hub.depth));
    out.depth = static_cast<uint8_t>(hub.depth + 1);
    out.rootPort = hub.rootPort;
    out.speed = childSpeed;
    if (UsbSpeedNeedsTt(childSpeed)) {
        if (hub.speed == UsbSpeedHigh) {
            out.ttSlot = hubSlot; out.ttPort = port; out.ttMulti = hubMulti;
        } else {
            // A full-speed hub behind a high-speed hub shares that TT.
            out.ttSlot = hub.ttSlot; out.ttPort = hub.ttPort; out.ttMulti = hub.ttMulti;
        }
    }
    return true;
}

// Slot context DW0..DW2 (xHCI 1.2 §6.2.2). Hub fields only for hubs.
struct UsbSlotWords { uint32_t dw0, dw1, dw2; };
inline UsbSlotWords UsbSlotContext(const UsbTopology& t, uint32_t contextEntries,
                                   bool hub, uint8_t hubPorts, uint8_t hubThinkTime, bool hubMulti) {
    UsbSlotWords w{};
    w.dw0 = (t.route & 0xfffffu) | ((t.speed & 15u) << 20) | ((contextEntries & 31u) << 27);
    if (hub) w.dw0 |= 1u << 26;
    // MTT: a multi-TT hub itself, or a LS/FS device behind a multi-TT hub.
    if ((hub && hubMulti && t.speed == UsbSpeedHigh) || (t.ttSlot && t.ttMulti)) w.dw0 |= 1u << 25;
    w.dw1 = (uint32_t(t.rootPort) << 16) | (hub ? uint32_t(hubPorts) << 24 : 0u);
    if (t.ttSlot) w.dw2 = t.ttSlot | (uint32_t(t.ttPort) << 8);
    if (hub && t.speed == UsbSpeedHigh) w.dw2 |= uint32_t(hubThinkTime & 3u) << 16;
    return w;
}

// Hub class port status (USB 2.0 §11.24.2.7, USB 3.2 §10.16.2.6).
struct UsbHubPortState {
    bool connected, enabled, resetting, powered, overCurrent;
    uint8_t linkState;   // SuperSpeed hubs only
    uint32_t speed;      // xHCI speed ID of the attached device
};
inline UsbHubPortState UsbHubDecodePort(uint16_t status, bool superSpeedHub) {
    UsbHubPortState s{};
    s.connected = status & 1u;
    s.enabled = status & 2u;
    s.overCurrent = status & 8u;
    s.resetting = status & 16u;
    if (superSpeedHub) {
        s.linkState = static_cast<uint8_t>((status >> 5) & 15u);
        s.powered = status & (1u << 9);
        s.speed = UsbSpeedSuper;
    } else {
        s.powered = status & (1u << 8);
        s.speed = (status & (1u << 9)) ? UsbSpeedLow : (status & (1u << 10)) ? UsbSpeedHigh : UsbSpeedFull;
    }
    return s;
}

// Hub port features (USB 2.0 Table 11-17, USB 3.2 Table 10-9).
enum : uint16_t {
    UsbHubFeatPortReset = 4, UsbHubFeatPortPower = 8,
    UsbHubFeatCConnection = 16, UsbHubFeatCEnable = 17, UsbHubFeatCSuspend = 18,
    UsbHubFeatCOverCurrent = 19, UsbHubFeatCReset = 20,
    UsbHubFeatCLinkState = 25, UsbHubFeatCConfigError = 26,
    UsbHubFeatBhPortReset = 28, UsbHubFeatCBhReset = 29,
};
// Port change bits that are set and must be acknowledged, as features.
inline uint32_t UsbHubChangeFeatures(uint16_t change, bool superSpeedHub, uint16_t* out, uint32_t capacity) {
    uint32_t n = 0;
    auto add = [&](uint16_t f) { if (n < capacity) out[n++] = f; };
    if (change & 1u) add(UsbHubFeatCConnection);
    if (!superSpeedHub && (change & 2u)) add(UsbHubFeatCEnable);
    if (!superSpeedHub && (change & 4u)) add(UsbHubFeatCSuspend);
    if (change & 8u) add(UsbHubFeatCOverCurrent);
    if (change & 16u) add(UsbHubFeatCReset);
    if (superSpeedHub) {
        if (change & 32u) add(UsbHubFeatCBhReset);
        if (change & 64u) add(UsbHubFeatCLinkState);
        if (change & 128u) add(UsbHubFeatCConfigError);
    }
    return n;
}

// Endpoint context Interval (xHCI 1.2 §6.2.3.6) from bInterval.
inline bool UsbXhciInterval(uint32_t speed, uint8_t bInterval, uint32_t& out) {
    if (!bInterval) return false;
    if (speed == UsbSpeedHigh || UsbSpeedIsSuper(speed)) {
        if (bInterval > 16) return false;
        out = bInterval - 1u;
        return true;
    }
    // LS/FS: bInterval frames (1..255) -> 2^out microframes, rounded down.
    uint32_t exponent = 0;
    for (uint32_t v = bInterval; v > 1; v >>= 1) ++exponent;
    out = exponent + 3;
    if (out > 10) out = 10;
    return true;
}

// Default EP0 max packet before the device descriptor is read.
constexpr uint32_t UsbDefaultEp0Packet(uint32_t speed) {
    return UsbSpeedIsSuper(speed) ? 512u : speed == UsbSpeedLow ? 8u : 64u;
}
// EP0 max packet from bMaxPacketSize0 (an exponent for SuperSpeed).
constexpr uint32_t UsbEp0PacketFromDescriptor(uint32_t speed, uint8_t value) {
    return UsbSpeedIsSuper(speed) ? (value >= 5 && value <= 9 ? (1u << value) : 512u)
                                  : (value == 8 || value == 16 || value == 32 || value == 64 ? value : 0u);
}

// Root port decision for one PORTSC sample (xHCI 1.2 §4.19.1).
enum class XhciRootAction { Idle, Enumerate, Reset, WarmReset, Wait };
inline XhciRootAction XhciRootPortAction(uint32_t portsc, bool usb3Port) {
    constexpr uint32_t connected = 1u, enabled = 2u, resetting = 1u << 4;
    const uint32_t pls = (portsc >> 5) & 15u;
    if (portsc == 0xffffffffu) return XhciRootAction::Idle;
    if (portsc & resetting) return XhciRootAction::Wait;
    if (usb3Port) {
        // Compliance / SS.Inactive need a warm reset even without CCS.
        if (pls == 10 || pls == 6) return XhciRootAction::WarmReset;
        if (!(portsc & connected)) return XhciRootAction::Idle;
        if (portsc & enabled) return XhciRootAction::Enumerate;
        if (pls == 5 || pls == 7 || pls == 8 || pls == 9 || pls == 15) return XhciRootAction::Wait; // training
        return XhciRootAction::WarmReset;
    }
    if (!(portsc & connected)) return XhciRootAction::Idle;
    return (portsc & enabled) ? XhciRootAction::Enumerate : XhciRootAction::Reset;
}

constexpr const char* UsbSpeedName(uint32_t speed) {
    return speed == UsbSpeedFull ? "FS" : speed == UsbSpeedLow ? "LS" : speed == UsbSpeedHigh ? "HS" :
           speed == UsbSpeedSuper ? "SS" : speed == UsbSpeedSuperPlus ? "SS+" : "?";
}
constexpr const char* XhciLinkStateName(uint32_t pls) {
    constexpr const char* names[16] = {"U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive",
        "Polling", "Recovery", "HotReset", "Compliance", "Test", "12", "13", "14", "Resume"};
    return names[pls & 15u];
}
}

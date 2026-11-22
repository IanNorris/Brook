#pragma once

#include <stdint.h>

namespace brook {

inline constexpr unsigned HdaNodeCount = 256;
inline constexpr uint8_t HdaParamWidgetCaps = 0x09;
inline constexpr uint8_t HdaParamPinCaps = 0x0c;
inline constexpr uint8_t HdaParamInputAmpCaps = 0x0d;
inline constexpr uint8_t HdaParamConnections = 0x0e;
inline constexpr uint8_t HdaParamOutputAmpCaps = 0x12;
inline constexpr uint16_t HdaGetParameter = 0x0f00;
inline constexpr uint16_t HdaGetConnections = 0x0f02;
inline constexpr uint16_t HdaGetPinDefault = 0x0f1c;

inline constexpr uint32_t HdaWidgetStereo = 1u;
inline constexpr uint32_t HdaWidgetInputAmp = 1u << 1;
inline constexpr uint32_t HdaWidgetOutputAmp = 1u << 2;
inline constexpr uint32_t HdaWidgetAmpOverride = 1u << 3;
inline constexpr uint32_t HdaWidgetConnections = 1u << 8;
inline constexpr uint32_t HdaWidgetDigital = 1u << 9;
inline constexpr uint32_t HdaWidgetPower = 1u << 10;

enum class HdaWidgetType : uint8_t { Dac = 0, Mixer = 2, Selector = 3, Pin = 4 };

inline HdaWidgetType HdaWidgetKind(uint32_t caps)
{
    return static_cast<HdaWidgetType>((caps >> 20) & 0xf);
}

struct HdaPlaybackRoute {
    struct Step {
        uint8_t node = 0;
        uint8_t input = 0;
        bool selectInput = false;
    };
    // The first step is the DAC and the last is the analog output pin.
    Step steps[16]{};
    unsigned count = 0;
    uint32_t pinCaps = 0;
};

template <class Read>
unsigned HdaReadConnections(Read read, uint8_t node, uint8_t (&sources)[HdaNodeCount])
{
    uint32_t parameter = 0;
    if (!read(node, HdaGetParameter, HdaParamConnections, parameter) || parameter > 0xff)
        return 0;

    const unsigned entries = parameter & 0x7f;
    const unsigned bits = parameter & 0x80 ? 16 : 8;
    const unsigned range = 1u << (bits - 1);
    const unsigned perRead = 32 / bits;
    unsigned count = 0, previous = 0;
    uint32_t packed = 0;
    for (unsigned index = 0; index < entries; ++index) {
        if (index % perRead == 0 &&
            !read(node, HdaGetConnections, static_cast<uint8_t>(index), packed))
            return 0;
        const unsigned entry = packed & ((1u << bits) - 1);
        packed >>= bits;
        const unsigned last = entry & (range - 1);
        if (!last || last >= HdaNodeCount || ((entry & range) && (!previous || last <= previous)))
            return 0;
        const unsigned first = entry & range ? previous + 1 : last;
        for (unsigned source = first; source <= last; ++source) {
            if (count == HdaNodeCount) return 0;
            sources[count++] = static_cast<uint8_t>(source);
        }
        previous = last;
    }
    return count;
}

template <class Read>
HdaPlaybackRoute HdaFindPlaybackRoute(const uint32_t (&widgets)[HdaNodeCount], Read read)
{
    for (unsigned pin = 1; pin < HdaNodeCount; ++pin) {
        const uint32_t pinWidget = widgets[pin];
        if (HdaWidgetKind(pinWidget) != HdaWidgetType::Pin ||
            (pinWidget & (HdaWidgetDigital | HdaWidgetConnections)) != HdaWidgetConnections)
            continue;
        uint32_t pinDefault = 0, pinCaps = 0;
        const auto pinNode = static_cast<uint8_t>(pin);
        if (!read(pinNode, HdaGetPinDefault, 0, pinDefault) ||
            ((pinDefault >> 30) & 3) == 1 || ((pinDefault >> 20) & 0xf) > 2 ||
            !read(pinNode, HdaGetParameter, HdaParamPinCaps, pinCaps) || !(pinCaps & (1u << 4)))
            continue;

        // Breadth-first from the pin back to a DAC. Each NID is queued at
        // most once, so a cyclic codec graph cannot loop.
        uint8_t parents[HdaNodeCount]{}, inputs[HdaNodeCount]{}, queue[HdaNodeCount]{};
        bool selectors[HdaNodeCount]{};
        parents[pin] = pinNode;
        queue[0] = pinNode;
        unsigned tail = 1;
        for (unsigned head = 0; head < tail; ++head) {
            const uint8_t node = queue[head];
            const uint32_t caps = widgets[node];
            const auto type = HdaWidgetKind(caps);
            if (type == HdaWidgetType::Dac) {
                HdaPlaybackRoute route{};
                route.pinCaps = pinCaps;
                uint8_t part = node;
                for (;;) {
                    route.steps[route.count++].node = part;
                    if (part == pinNode) return route;
                    if (route.count == sizeof(route.steps) / sizeof(route.steps[0])) break;
                    const auto source = part;
                    part = parents[source];
                    route.steps[route.count].input = inputs[source];
                    route.steps[route.count].selectInput = selectors[source];
                }
                continue;
            }
            if (!(caps & HdaWidgetConnections)) continue;
            uint8_t sources[HdaNodeCount];
            const unsigned count = HdaReadConnections(read, node, sources);
            for (unsigned i = 0; i < count; ++i) {
                const auto source = sources[i];
                const uint32_t sourceCaps = widgets[source];
                const auto sourceKind = HdaWidgetKind(sourceCaps);
                if (parents[source] || (sourceCaps & HdaWidgetDigital) ||
                    ((caps & HdaWidgetInputAmp) && i > 15) ||
                    (sourceKind == HdaWidgetType::Dac && !(sourceCaps & HdaWidgetStereo)) ||
                    (sourceKind != HdaWidgetType::Dac && sourceKind != HdaWidgetType::Mixer &&
                     sourceKind != HdaWidgetType::Selector))
                    continue;
                parents[source] = node;
                inputs[source] = static_cast<uint8_t>(i);
                selectors[source] = count > 1 && type != HdaWidgetType::Mixer;
                queue[tail++] = source;
            }
        }
    }
    return {};
}

} // namespace brook

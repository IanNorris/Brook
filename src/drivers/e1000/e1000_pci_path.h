#pragma once

#include <stdint.h>

namespace brook::e1000 {

struct Bridge {
    uint8_t bus, device, function, secondary, subordinate;
};

// Firmware programs PCI bus numbers and address windows. Find the one
// configured path carrying this endpoint before changing any commands.
template <typename ReadConfig>
bool FindBridgePath(uint8_t endpoint, ReadConfig read, Bridge (&path)[32], unsigned& count)
{
    count = 0;
    uint8_t bus = 0, limit = 255;
    bool visited[256]{};
    while (bus != endpoint) {
        if (count == 32 || visited[bus]) return false;
        visited[bus] = true;
        bool found = false;
        Bridge bridge{};
        for (uint8_t device = 0; device < 32; ++device) {
            if ((read(bus, device, 0, 0) & 0xffffu) == 0xffffu) continue;
            const uint8_t functions = (read(bus, device, 0, 0x0c) & (1u << 23)) ? 8 : 1;
            for (uint8_t function = 0; function < functions; ++function) {
                if ((read(bus, device, function, 0x08) >> 16) != 0x0604 ||
                    ((read(bus, device, function, 0x0c) >> 16) & 0x7fu) != 1)
                    continue;
                const uint32_t numbers = read(bus, device, function, 0x18);
                const uint8_t primary = numbers, secondary = numbers >> 8, subordinate = numbers >> 16;
                if (primary != bus || !secondary || secondary > endpoint ||
                    subordinate < endpoint || subordinate > limit) continue;
                if (found) return false;
                found = true;
                bridge = {bus, device, function, secondary, subordinate};
            }
        }
        if (!found) return false;
        bus = bridge.secondary;
        limit = bridge.subordinate;
        path[count++] = bridge;
    }
    return true;
}

}

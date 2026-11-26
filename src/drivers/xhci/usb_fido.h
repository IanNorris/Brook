#pragma once

#include <stddef.h>
#include <stdint.h>

namespace brook {

inline constexpr uint16_t UsbFidoUsagePage = 0xf1d0;
inline constexpr unsigned UsbFidoReportBytes = 64;
inline constexpr unsigned UsbFidoDescriptorLimit = 256;

struct UsbFidoInterface {
    uint8_t number = 0;
    uint8_t in = 0, out = 0;
    uint8_t inInterval = 0, outInterval = 0;
    uint16_t descriptorBytes = 0;
};

// Locate a pair of fixed-size interrupt endpoints belonging to one non-boot
// HID interface. Merely finding these endpoints does not authorize claiming it.
inline bool UsbFidoFindInterface(const uint8_t* config, size_t bytes, unsigned first,
                                 UsbFidoInterface& result)
{
    UsbFidoInterface candidate{};
    bool candidateActive = false;
    bool ambiguous = false;
    auto accept = [&] {
        if (!candidateActive || ambiguous || !candidate.in || !candidate.out ||
            !candidate.descriptorBytes || candidate.descriptorBytes > UsbFidoDescriptorLimit)
            return false;
        result = candidate;
        return true;
    };
    for (size_t i = 0; i + 2 <= bytes;) {
        const uint8_t* d = config + i;
        const unsigned length = d[0];
        if (length < 2 || length > bytes - i) return false;
        i += length;
        if (d[1] == 4) {
            if (accept()) return true;
            candidate = {};
            ambiguous = false;
            candidateActive = length >= 9 && d[2] >= first && d[3] == 0 &&
                              d[5] == 3 && d[6] == 0 && d[7] == 0;
            if (candidateActive) candidate.number = d[2];
        } else if (candidateActive && d[1] == 0x21 && length >= 6) {
            unsigned entries = d[5];
            if (entries > (length - 6) / 3) return false;
            for (unsigned j = 0; j < entries; ++j) {
                const uint8_t* entry = d + 6 + j * 3;
                if (entry[0] != 0x22) continue;
                if (candidate.descriptorBytes) ambiguous = true;
                candidate.descriptorBytes = entry[1] | (uint16_t(entry[2]) << 8);
            }
        } else if (candidateActive && d[1] == 5 && length >= 7) {
            if ((d[3] & 3) != 3) continue;
            if ((d[2] & 0x70) || !(d[2] & 15) || d[4] != UsbFidoReportBytes || d[5] || !d[6]) {
                ambiguous = true;
                continue;
            }
            uint8_t& address = (d[2] & 0x80) ? candidate.in : candidate.out;
            uint8_t& interval = (d[2] & 0x80) ? candidate.inInterval : candidate.outInterval;
            if (address) ambiguous = true;
            address = d[2];
            interval = d[6];
        }
    }
    return accept();
}

// CTAPHID uses a single authenticator application collection, an unnumbered
// input/output report and one byte per field. Reject other collections, feature
// reports and alternate report IDs rather than exposing an unrelated function.
inline bool UsbFidoReportDescriptor(const uint8_t* data, size_t bytes)
{
    if (!data || !bytes || bytes > UsbFidoDescriptorLimit) return false;
    uint32_t page = 0, usage = 0, size = 0, count = 0;
    bool collection = false, closed = false, input = false, output = false;
    for (size_t i = 0; i < bytes;) {
        const uint8_t tag = data[i++];
        unsigned length = tag & 3u;
        if (length == 3) length = 4;
        if (tag == 0xfe || length > bytes - i || closed) return false;
        uint32_t value = 0;
        for (unsigned j = 0; j < length; ++j) value |= uint32_t(data[i++]) << (8 * j);
        switch (tag & 0xfcu) {
        case 0x04: page = value; break;
        case 0x08: usage = value; break;
        case 0x74: size = value; break;
        case 0x94: count = value; break;
        case 0x84: case 0xa4: case 0xb4: case 0xb0: return false;
        case 0xa0:
            if (collection || page != UsbFidoUsagePage || usage != 1 || value != 1) return false;
            collection = true;
            usage = 0;
            break;
        case 0x80: case 0x90: {
            bool isInput = (tag & 0xfcu) == 0x80;
            bool& seen = isInput ? input : output;
            if (!collection || seen || page != UsbFidoUsagePage || size != 8 ||
                count != UsbFidoReportBytes || usage != (isInput ? 0x20u : 0x21u) ||
                (value & 7) != 2) return false;
            seen = true;
            usage = 0;
            break;
        }
        case 0xc0:
            if (!collection || length) return false;
            closed = true;
            break;
        default:
            if ((tag & 0x0c) == 0) return false;
            break;
        }
    }
    return closed && input && output;
}

} // namespace brook

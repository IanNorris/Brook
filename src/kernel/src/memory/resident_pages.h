#pragma once
#include "mapping_ranges.h"

namespace brook {
struct ResidentPages {
    uint64_t virtualPages = 0;
    uint64_t anonymous = 0, file = 0, shared = 0;

    static bool ChargesRss(const MappingRange& range)
    {
        // Device mappings may point to MMIO and are reported by the
        // device's own accounting, not charged as process RSS.
        return range.kind != MappingKind::Device;
    }

    void Add(const MappingRange& range, uint64_t backed)
    {
        virtualPages += (range.end - range.begin) / 4096;
        switch (range.kind) {
        case MappingKind::File:
        case MappingKind::Image:
            file += backed;
            break;
        case MappingKind::MemFd:
            shared += backed;
            break;
        case MappingKind::Pageable:
        case MappingKind::Anonymous:
        case MappingKind::Stack:
        case MappingKind::Heap:
            anonymous += backed;
            break;
        case MappingKind::Device:
            break;
        }
    }

    uint64_t Total() const { return anonymous + file + shared; }
};
} // namespace brook

#pragma once
#include "ssd_install_plan.h"
#include <limits.h>

namespace brook {
// Maps the three ranges the install plan actually reads into a compact source:
// primary GPT (including MBR), payload, then backup GPT. The caller must pin
// and authenticate the complete packed source, verify the GPT against the
// same geometry, and authorize an exclusive target before any disk write.
// Reads through gaps fail instead of silently supplying invented data.
class SsdInstallCompactSource {
    SsdInstallGeometry geometry_;
    uint64_t packedBytes_;
    bool Region(uint64_t position, uint64_t& packed, uint64_t& available) const {
        const uint64_t primaryEnd = 34ull * 512;
        const uint64_t payloadStart = geometry_.payloadFirst * 512;
        const uint64_t payloadEnd = payloadStart + geometry_.payloadSectors * 512;
        const uint64_t backupStart = (geometry_.diskSectors - 33) * 512;
        const uint64_t diskEnd = geometry_.diskSectors * 512;
        if (position < primaryEnd) { packed = position; available = primaryEnd - position; return true; }
        if (position >= payloadStart && position < payloadEnd) {
            packed = primaryEnd + position - payloadStart; available = payloadEnd - position; return true;
        }
        if (position >= backupStart && position < diskEnd) {
            packed = primaryEnd + geometry_.payloadSectors * 512 + position - backupStart;
            available = diskEnd - position; return true;
        }
        return false;
    }
public:
    SsdInstallCompactSource(SsdInstallGeometry geometry, uint64_t packedBytes)
        : geometry_(geometry), packedBytes_(packedBytes) {}
    uint64_t ExpectedSize() const {
        return SsdInstallGeometryValid(geometry_) ? (34 + geometry_.payloadSectors + 33) * 512 : 0;
    }
    bool Valid() const { return ExpectedSize() && packedBytes_ == ExpectedSize(); }
    template<class Reader> int Read(Reader& reader, uint64_t position, void* dest, uint32_t bytes) const {
        if (!Valid() || bytes > INT_MAX || (!dest && bytes) || position > geometry_.diskSectors * 512 ||
            bytes > geometry_.diskSectors * 512 - position) return -1;
        // Validate the entire logical range before invoking the reader.
        uint32_t checked = 0;
        while (checked < bytes) {
            uint64_t packed, available;
            if (!Region(position + checked, packed, available)) return -1;
            checked += uint32_t(available < bytes - checked ? available : bytes - checked);
        }
        auto* output = static_cast<uint8_t*>(dest);
        uint32_t done = 0;
        while (done < bytes) {
            uint64_t packed, available;
            if (!Region(position + done, packed, available)) return -1;
            const uint32_t n = uint32_t(available < bytes - done ? available : bytes - done);
            if (reader.readPacked(packed, output + done, n) != int(n)) return -1;
            done += n;
        }
        return int(done);
    }
};
}

#pragma once
#include "desktop_metrics.h"

namespace brook {

// Only the current page crosses to the shell process; the worker retains
// the entire initial-namespace volume list independently of display size.
inline constexpr uint32_t DiskPopupPageRows = 10;

struct DesktopDiskVolume {
    char path[96];
    uint64_t freeBytes, totalBytes;
    bool available;
};

struct DesktopDiskPage {
    DesktopDiskVolume rows[DiskPopupPageRows];
    uint32_t first, count, total;
    bool measured;
};

inline bool DesktopDiskNameIs(const char* value, const char* expected) {
    while (*value && *value == *expected) { ++value; ++expected; }
    return *value == *expected;
}

inline bool DesktopDiskVolumeVisible(const char* path, const char* fs, bool systemInternal) {
    if (systemInternal || *path != '/' || !path[1]) return false; // '/' is the RAM initrd.
    const char* virtualTypes[] = {"proc", "procfs", "sysfs", "tmpfs", "devtmpfs", "devpts", "mqueue"};
    for (const char* type : virtualTypes)
        if (DesktopDiskNameIs(fs, type)) return false;
    return true;
}

struct DesktopDiskIdentity {
    uint64_t type, identity;
    char fsType[16];
    uint8_t drive;
};

inline bool DesktopDiskSameVolume(const DesktopDiskIdentity& a, const DesktopDiskIdentity& b) {
    if (a.identity && b.identity) return a.type == b.type && a.identity == b.identity;
    return a.drive == b.drive && DesktopDiskNameIs(a.fsType, b.fsType);
}

inline bool DesktopDiskCapacity(uint64_t blockSize, uint64_t blocks, uint64_t available,
                                uint64_t& totalBytes, uint64_t& freeBytes) {
    if (!blockSize || available > blocks || blocks > ~uint64_t{0} / blockSize) return false;
    totalBytes = blocks * blockSize;
    freeBytes = available * blockSize;
    return true;
}

inline uint32_t DesktopDiskPageFirst(uint32_t first, uint32_t total, uint32_t page) {
    const uint32_t last = total > page ? total - page : 0;
    return first < last ? first : last;
}

inline uint32_t DesktopDiskPageStep(uint32_t first, uint32_t total, uint32_t page, int64_t step) {
    first = DesktopDiskPageFirst(first, total, page);
    const uint32_t last = total > page ? total - page : 0;
    if (step <= -int64_t(first)) return 0;
    if (step >= int64_t(last - first)) return last;
    return uint32_t(int64_t(first) + step);
}

void DesktopDiskWorkerStart();
void DesktopDiskSetVisible(bool visible);
uint32_t DesktopDiskVolumeCount();
uint32_t DesktopDiskReadPage(uint32_t first, uint32_t capacity, DesktopDiskPage& out);

} // namespace brook

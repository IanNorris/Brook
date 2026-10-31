#include "hardware_store_validate.h"
#include "../src/kernel/src/writable_desktop_plan.h"
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

static bool read_exact(int fd, uint64_t offset, void *buffer, size_t bytes) {
    auto *out = static_cast<uint8_t *>(buffer);
    while (bytes) {
        ssize_t n = pread(fd, out, bytes, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        out += n; offset += static_cast<uint64_t>(n); bytes -= static_cast<size_t>(n);
    }
    return true;
}

extern "C" int hardware_store_validate(int fd, const char *record, size_t record_bytes) {
    brook::WritableDesktopPlan plan{};
    if (record_bytes > 96 || !brook::WritableDesktopPlanParse(record, static_cast<uint32_t>(record_bytes), plan)) return -EINVAL;
    struct stat st{};
    if (fstat(fd, &st)) return -errno;
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || static_cast<uint64_t>(st.st_size) != plan.bytes) return -EINVAL;
    uint8_t sb[1024]{}, gdt[1024]{};
    if (!read_exact(fd, 1024, sb, sizeof(sb))) return -EIO;
    if (!brook::WritableDesktopHeaderValid(sb, plan)) return -EINVAL;
    const uint32_t bytes = ((brook::StoreGet32(sb+4)+32767)/32768)*32;
    if (!read_exact(fd, 4096, gdt, bytes)) return -EIO;
    return brook::WritableDesktopGroupsValid(sb, gdt, bytes) ? 0 : -EINVAL;
}

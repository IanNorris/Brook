// Synthetic /sys CPU lists and /proc cgroup text; Linux formats are the spec.
#include "../../kernel/src/synthetic_system_files.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
unsigned failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "SYNTHETIC FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
bool Is(const char* got, const char* want) { return got && !std::strcmp(got, want); }
}

int main()
{
    using brook::SysfsCpuAttribute;
    static_assert(brook::g_sysfsCpuLists.text[4][0] == '0' && brook::g_sysfsCpuLists.text[4][2] == '3');
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/possible", 4, 64), "0-3\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/present", 4, 64), "0-3\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/online", 4, 64), "0-3\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/online", 1, 64), "0\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/online", 0, 64), "0\n")); // before SMP
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/online", 2, 64), "0-1\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/online", 11, 64), "0-10\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/online", 64, 64), "0-63\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/online", 200, 64), "0-63\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/offline", 4, 64), "\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/kernel_max", 4, 64), "63\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/kernel_max", 4, 8), "7\n"));
    CHECK(Is(SysfsCpuAttribute("/sys/devices/system/cpu/kernel_max", 4, 1), "0\n"));
    for (const char* other : {"/sys/devices/system/cpu/possible/", "/sys/devices/system/cpu/possiblex",
                              "/sys/devices/system/cpu/cpu0", "/sys/devices/system/cpu", "/sys/devices/system/CPU/online",
                              "/proc/self/cgroup", "", "/sys/devices/system/cpu/"})
        CHECK(!SysfsCpuAttribute(other, 4, 64));
    CHECK(!SysfsCpuAttribute(nullptr, 4, 64));
    CHECK(!std::strcmp(brook::ProcCgroupText, "0::/\n"));
    std::printf("synthetic system files: %u failures\n", failures);
    return failures ? 1 : 0;
}

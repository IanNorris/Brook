#pragma once
// synthetic_system_files.h -- fixed-format /sys and /proc text Brook
// synthesizes for Linux software. Pure and host-testable.
#include <stdint.h>

namespace brook {

// Linux cpulist text ("0\n", "0-3\n") for CPUs [0,count). Brook brings CPUs
// up as a dense prefix and has no hotplug, so possible == present == online.
struct SysfsCpuLists {
    char text[65][8];
    constexpr SysfsCpuLists() : text{} {
        for (uint32_t count = 1; count <= 64; ++count) {
            char* out = text[count];
            uint32_t at = 0;
            out[at++] = '0';
            if (count > 1) {
                const uint32_t last = count - 1;
                out[at++] = '-';
                if (last >= 10) out[at++] = char('0' + last / 10);
                out[at++] = char('0' + last % 10);
            }
            out[at++] = '\n';
            out[at] = 0;
        }
        text[0][0] = '\n'; // an empty list (e.g. "offline")
    }
};
inline constexpr SysfsCpuLists g_sysfsCpuLists{};

inline bool SyntheticPathEqual(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

// Content for /sys/devices/system/cpu/{possible,present,online,offline,
// kernel_max}, or nullptr. maxCpus is the kernel's CPU limit (NR_CPUS).
inline const char* SysfsCpuAttribute(const char* path, uint32_t online, uint32_t maxCpus)
{
    static constexpr char prefix[] = "/sys/devices/system/cpu/";
    if (!path) return nullptr;
    uint32_t i = 0;
    for (; prefix[i]; ++i) if (path[i] != prefix[i]) return nullptr;
    const char* leaf = path + i;
    if (online < 1) online = 1;
    if (online > 64) online = 64;
    if (SyntheticPathEqual(leaf, "possible") || SyntheticPathEqual(leaf, "present") ||
        SyntheticPathEqual(leaf, "online"))
        return g_sysfsCpuLists.text[online];
    if (SyntheticPathEqual(leaf, "offline")) return g_sysfsCpuLists.text[0];
    if (SyntheticPathEqual(leaf, "kernel_max")) {
        // kernel_max is NR_CPUS-1 printed as a number; reuse the list's tail.
        if (maxCpus < 2 || maxCpus > 64) return "0\n";
        return g_sysfsCpuLists.text[maxCpus] + 2; // "0-63\n" -> "63\n"
    }
    return nullptr;
}

// /proc/<pid>/cgroup: every task is in the cgroup v2 root. Brook has no
// cgroup hierarchy, so consumers (libuv, Rust std, Firefox) find no limits.
inline constexpr char ProcCgroupText[] = "0::/\n";

} // namespace brook

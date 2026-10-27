#pragma once
#include <stdint.h>

namespace brook {
// The BSP must establish this itself; firmware may leave NX disabled. APs
// already enable NX in the long-mode trampoline before reading kernel PTEs.
template<class Cpuid, class ReadEfer, class WriteEfer>
bool CpuEnableExecuteDisable(Cpuid cpuid, ReadEfer read, WriteEfer write,
                             uint64_t& before, uint64_t& after)
{
    uint32_t a, b, c, d;
    cpuid(0x80000000u, a, b, c, d);
    if (a < 0x80000001u) return false;
    cpuid(0x80000001u, a, b, c, d);
    if (!(d & (1u << 20))) return false;
    constexpr uint64_t nxe = uint64_t{1} << 11;
    before = read();
    if (!(before & nxe)) write(before | nxe);
    after = read();
    return (after & nxe) && ((after & ~nxe) == (before & ~nxe));
}
}

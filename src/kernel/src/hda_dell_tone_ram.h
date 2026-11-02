#pragma once
#include "memory/kernel_alias_probe.h"
namespace brook {
// These pages came from PMM; the mapping check must accept 4 KiB, 2 MiB and
// 1 GiB direct-map leaves. This narrow diagnostic intentionally uses <4 GiB.
inline bool HdaDellToneRamMappingMatches(uint64_t physical,uint64_t address,uint64_t directBase,const KernelAliasMapping& direct) {
    if(!physical || physical>0xfffff000ULL || (physical&4095) || directBase>~uint64_t(0)-physical ||
       address!=directBase+physical)return false;
    return direct.present && (direct.pageShift==12 || direct.pageShift==21 || direct.pageShift==30) && direct.physical==physical;
}
}

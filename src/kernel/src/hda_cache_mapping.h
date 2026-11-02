#pragma once
#include "memory/kernel_alias_probe.h"
namespace brook {
// These validate the actual tracked aliases. They do not prove that no other
// aliases exist or determine effective memory types without the MTRR snapshot.
inline bool HdaCacheMmioMapping(uint64_t physical,const KernelAliasMapping& mapping) {
    return mapping.present && mapping.physical==physical && mapping.pageShift==12 && mapping.patIndex==3;
}
inline bool HdaCacheDmaMappings(uint64_t physical,const KernelAliasMapping& identity,const KernelAliasMapping& direct) {
    if(!direct.present || direct.physical!=physical || direct.patIndex!=0 ||
       (direct.pageShift!=12 && direct.pageShift!=21 && direct.pageShift!=30))return false;
    return !identity.present || (identity.physical==physical && identity.patIndex==0 &&
        (identity.pageShift==12 || identity.pageShift==21 || identity.pageShift==30));
}
}

#pragma once
#include <stdint.h>
#include "gdt.h"

namespace brook {
// Appended above the original 128-byte syscall frame. Stack-local on every
// entry, never inherited or shared through a process/per-CPU return-mode flag.
struct SyscallReturnState {
    uint64_t mode,rcx,rip,cs,flags,rsp,ss,padding;
};
static_assert(sizeof(SyscallReturnState)==64);
static_assert(__builtin_offsetof(SyscallReturnState,rcx)==8);
static_assert(__builtin_offsetof(SyscallReturnState,rip)==16);
inline bool PrepareSignalReturn(SyscallReturnState& state,uint64_t rip,uint64_t rsp,
                                uint64_t flags,uint64_t rcx) {
    constexpr uint64_t userLimit=0x0000800000000000ULL;
    if(rip<0x10000 || rsp<0x10000 || rip>=userLimit || rsp>=userLimit)return false;
    // User condition/debug/direction/alignment flags only. Force IF/bit1 and
    // clear IOPL, NT, VM, VIF, VIP and reserved bits before a CPL0 IRET.
    state={1,rcx,rip,GDT_USER_CODE,(flags&0x250dd5ULL)|0x202ULL,rsp,GDT_USER_DATA,0};
    return true;
}
}

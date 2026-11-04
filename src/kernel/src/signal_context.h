#pragma once
#include "process.h"
#include <stddef.h>

namespace brook {
// LAPIC timer/reschedule wrapper's 15 pushes followed by the CPU IRET frame.
// Only ring-3 frames are eligible for asynchronous user signal delivery.
struct IrqSignalFrame {
    uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax;
    uint64_t rip,cs,rflags,rsp,ss;
};
static_assert(offsetof(IrqSignalFrame,rbp)==64);
static_assert(offsetof(IrqSignalFrame,rax)==112);
static_assert(offsetof(IrqSignalFrame,rip)==120);
static_assert(offsetof(IrqSignalFrame,cs)==128);
static_assert(offsetof(IrqSignalFrame,rsp)==144);
static_assert(sizeof(IrqSignalFrame)==160);

inline void SignalContextFromSyscall(SignalMcontext& mc,const SyscallFrame& frame,
                                     uint64_t result,uint16_t cs,uint16_t ss) {
    mc.rax=result;mc.rbx=frame.rbx;mc.rcx=frame.rcx;mc.rdx=frame.rdx;
    mc.rsi=frame.rsi;mc.rdi=frame.rdi;mc.rbp=frame.rbp;mc.rsp=frame.rsp;
    mc.r8=frame.r8;mc.r9=frame.r9;mc.r10=frame.r10;mc.r11=frame.r11;
    mc.r12=frame.r12;mc.r13=frame.r13;mc.r14=frame.r14;mc.r15=frame.r15;
    mc.rip=frame.rcx;mc.eflags=frame.rflags;mc.cs=cs;mc.ss=ss;
}
inline bool SignalContextFromIrq(SignalMcontext& mc,const IrqSignalFrame& frame) {
    if((frame.cs&3)!=3)return false;
    mc.rax=frame.rax;mc.rbx=frame.rbx;mc.rcx=frame.rcx;mc.rdx=frame.rdx;
    mc.rsi=frame.rsi;mc.rdi=frame.rdi;mc.rbp=frame.rbp;mc.rsp=frame.rsp;
    mc.r8=frame.r8;mc.r9=frame.r9;mc.r10=frame.r10;mc.r11=frame.r11;
    mc.r12=frame.r12;mc.r13=frame.r13;mc.r14=frame.r14;mc.r15=frame.r15;
    mc.rip=frame.rip;mc.eflags=frame.rflags;mc.cs=frame.cs;mc.ss=frame.ss;
    return true;
}
}

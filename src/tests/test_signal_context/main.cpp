#include "signal_context.h"
#include <cassert>
#include <cstdio>
using namespace brook;
int main() {
    IrqSignalFrame irq{15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,101,0x23,0x202,102,0x1b};
    SignalMcontext context{};context.fpstate=0x1234;
    assert(SignalContextFromIrq(context,irq));
    assert(context.rax==1 && context.rbx==2 && context.rcx==3 && context.rdx==4);
    assert(context.rsi==5 && context.rdi==6 && context.rbp==7 && context.rsp==102);
    assert(context.r8==8 && context.r9==9 && context.r10==10 && context.r11==11);
    assert(context.r12==12 && context.r13==13 && context.r14==14 && context.r15==15);
    assert(context.rip==101 && context.eflags==0x202 && context.cs==0x23 && context.ss==0x1b && context.fpstate==0x1234);
    // A ring-0 interrupt must never be prepared as a user signal frame.
    IrqSignalFrame kernel{};kernel.cs=8;SignalMcontext unchanged{};unchanged.rax=0xbeef;
    assert(!SignalContextFromIrq(unchanged,kernel) && unchanged.rax==0xbeef);
    SyscallFrame syscall{};syscall.rcx=88;syscall.r11=99;syscall.rflags=0x246;syscall.rsp=77;
    SignalMcontext fromSyscall{};fromSyscall.fpstate=0x4567;
    SignalContextFromSyscall(fromSyscall,syscall,42,0x23,0x1b);
    assert(fromSyscall.rax==42 && fromSyscall.rcx==88 && fromSyscall.rip==88 && fromSyscall.r11==99);
    assert(fromSyscall.rsp==77 && fromSyscall.eflags==0x246 && fromSyscall.fpstate==0x4567);
    std::puts("SIGNAL_CONTEXT_PASS syscall/irq-registers/kernel-frame-guard");
}

// syscall.cpp — SYSCALL/SYSRET infrastructure
//
// This file provides the syscall entry stub and the initial (empty) syscall
// table.  The real dispatcher will be written in assembly once the per-CPU
// environment (KernelCpuEnv) and process model are ready.

#include "cpu.h"
#include "serial.h"

// Placeholder syscall entry point.  LSTAR points here.
// For now, this is a naked function that just panics — we haven't set up
// the dispatcher, GS swap, or stack switch yet.  This exists solely so
// CpuInitSyscallMsrs() has a valid address to write to LSTAR.
//
// When a user-mode process attempts SYSCALL before the real dispatcher is
// ready, this will fire and halt the machine.
__attribute__((naked)) static void SyscallEntryStub()
{
    __asm__ volatile(
        "cli\n\t"
        "1: hlt\n\t"
        "jmp 1b\n\t"
    );
}

// Return the address of the syscall entry stub for LSTAR.
uint64_t SyscallGetEntryStub()
{
    return reinterpret_cast<uint64_t>(&SyscallEntryStub);
}

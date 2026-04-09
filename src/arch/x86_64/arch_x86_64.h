#pragma once

// arch/x86_64/arch_x86_64.h — x86-64-specific inline implementations of
// the arch abstraction interface (arch/common/arch.h).
//
// These are inlined to avoid function-call overhead in hot paths like
// TlbInvalidatePage() and SpinHint().

#include "../common/arch.h"
#include <stdint.h>

namespace brook::arch {

inline void DisableInterrupts() { __asm__ volatile("cli" ::: "memory"); }
inline void EnableInterrupts()  { __asm__ volatile("sti" ::: "memory"); }

[[noreturn]] inline void HaltForever()
{
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

inline void MemoryBarrier() { __asm__ volatile("mfence" ::: "memory"); }
inline void StoreBarrier()  { __asm__ volatile("sfence" ::: "memory"); }
inline void LoadBarrier()   { __asm__ volatile("lfence" ::: "memory"); }

inline void TlbInvalidatePage(uint64_t virtAddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virtAddr) : "memory");
}

inline void TlbFlushAll()
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

inline uint64_t ReadPageTableRoot()
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

inline void WritePageTableRoot(uint64_t physAddr)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(physAddr) : "memory");
}

inline void SpinHint() { __asm__ volatile("pause"); }

inline uint32_t CurrentPrivilegeLevel() { return 0; }

// ---- x86-64-specific: port I/O ----
// These have no ARM64 equivalent; callers must be conditionally compiled.

inline void IoOutB(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline uint8_t IoInB(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

inline void IoOutL(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

inline uint32_t IoInL(uint16_t port)
{
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

inline void IoOutW(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

inline uint16_t IoInW(uint16_t port)
{
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

// ---- x86-64-specific: MSR access ----

inline uint64_t RdMsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline void WrMsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"(static_cast<uint32_t>(val)),
                     "d"(static_cast<uint32_t>(val >> 32)));
}

// ---- x86-64-specific: control registers ----

inline uint64_t ReadCr0() { uint64_t v; __asm__ volatile("mov %%cr0,%0":"=r"(v)); return v; }
inline void     WriteCr0(uint64_t v) { __asm__ volatile("mov %0,%%cr0"::"r"(v)); }
inline uint64_t ReadCr2() { uint64_t v; __asm__ volatile("mov %%cr2,%0":"=r"(v)); return v; }
inline uint64_t ReadCr4() { uint64_t v; __asm__ volatile("mov %%cr4,%0":"=r"(v)); return v; }
inline void     WriteCr4(uint64_t v) { __asm__ volatile("mov %0,%%cr4"::"r"(v)); }

} // namespace brook::arch

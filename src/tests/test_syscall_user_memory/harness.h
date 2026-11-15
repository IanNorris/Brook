#pragma once
// Host harness for real syscall handlers. Each test TU includes one
// src/kernel/src/syscall_*.cpp after this header and stubs only what the
// handlers it calls reach; --gc-sections drops everything else.
//
// User memory is a host mmap arena with per-page protection. Every address
// outside it counts as kernel memory: the user-copy stubs refuse it exactly as
// the kernel's user_copy.cpp refuses the upper half. A handler that writes
// through a raw pointer therefore corrupts a host "kernel" canary instead of
// failing with -EFAULT, which the tests detect.
//
// Kernel string.h must win over libc, and pipe.h only selects it when
// __BROOK_KERNEL__ is defined, so include them in this order.
#include "spinlock.h"
#include "string.h"
#define __BROOK_KERNEL__
#include "pipe.h"
#undef __BROOK_KERNEL__
#include "apic.h"
#include "process.h"
#include "net.h"
#include <stdint.h>

namespace harness {
enum class Access : uint8_t { None, Read, ReadWrite };
constexpr uint64_t PageSize=4096;
constexpr uint64_t ArenaPages=64;

void Reset();                       // all pages read/write, zero filled
uint64_t UserBase();                // first arena address
uint64_t UserAddress(uint64_t offset);
void Protect(uint64_t offset,uint64_t length,Access access);
bool InArena(uint64_t address,uint64_t length,Access need);

// A host buffer standing in for kernel memory; never inside the arena.
struct KernelCanary {
    static constexpr uint64_t Size=8192;
    alignas(4096) uint8_t bytes[Size];
    void Fill();
    bool Intact() const;
    uint64_t Address() const { return reinterpret_cast<uint64_t>(bytes); }
};
extern KernelCanary g_kernel;

brook::Process* CurrentProcess();   // zeroed, PROCESS_MAGIC, pid 7
void ResetProcess();

// Count of user-copy calls that were refused because they hit kernel memory.
extern unsigned g_refusedKernelCopies;

// Fake network stack shared by the socket and read() tests.
struct FakeNet {
    unsigned binds=0,sends=0,receives=0,accepts=0;
    brook::SockAddrIn bound{},sentDestination{};
    bool sentWithDestination=false;
    uint8_t sent[256]{};uint32_t sentLength=0;
    bool datagramReady=false;uint8_t datagram[64]{};uint32_t datagramLength=0;
    brook::SockAddrIn source{},peer{};
    bool acceptReady=false;
    uint32_t localIp=0,remoteIp=0;uint16_t localPort=0,remotePort=0;
};
extern FakeNet g_net;
extern unsigned g_snapshots;   // ProcessFsSnapshot calls (open lookups)

void Check(bool condition,const char* expression,const char* file,int line);
unsigned Failures();
}
#define CHECK(condition) ::harness::Check((condition),#condition,__FILE__,__LINE__)

// Each TU registers its cases through one function called from main.cpp.
void RunInputWmTests();
void RunDirOpenTests();
void RunRwTests();
void RunSocketTests();
void RunUnixTests();
void RunProcessTests();

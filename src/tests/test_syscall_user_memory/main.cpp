// Shared host stand-ins: user memory, the current process, the fd table pin
// and logging. Handler-specific dependencies live in the per-file TUs.
#include "harness.h"
#include "user_copy.h"
#include "user_access_scope.h"
#include "fd_table.h"
#include "syscall_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

namespace harness {
namespace {
uint8_t* g_arena;
Access g_pages[ArenaPages];
brook::Process* g_process;
brook::FdEntry g_fds[brook::MAX_FDS];
unsigned g_failures;
}
KernelCanary g_kernel;
unsigned g_refusedKernelCopies;

void KernelCanary::Fill() { for(uint64_t i=0;i<Size;++i)bytes[i]=static_cast<uint8_t>(0xA5^i); }
bool KernelCanary::Intact() const {
    for(uint64_t i=0;i<Size;++i)if(bytes[i]!=static_cast<uint8_t>(0xA5^i))return false;
    return true;
}

void Reset() {
    if(!g_arena) {
        void* p=mmap(nullptr,ArenaPages*PageSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(p==MAP_FAILED)abort();
        g_arena=static_cast<uint8_t*>(p);
    }
    for(uint64_t i=0;i<ArenaPages*PageSize;++i)g_arena[i]=0;
    for(auto& page:g_pages)page=Access::ReadWrite;
    g_kernel.Fill();
    g_refusedKernelCopies=0;
}
uint64_t UserBase() { return reinterpret_cast<uint64_t>(g_arena); }
uint64_t UserAddress(uint64_t offset) { return UserBase()+offset; }
void Protect(uint64_t offset,uint64_t length,Access access) {
    for(uint64_t page=offset/PageSize;page<(offset+length+PageSize-1)/PageSize && page<ArenaPages;++page)
        g_pages[page]=access;
}
bool InArena(uint64_t address,uint64_t length,Access need) {
    const uint64_t base=UserBase();
    if(address<base || length>ArenaPages*PageSize || address-base>ArenaPages*PageSize-length)return false;
    if(!length)return true;
    for(uint64_t page=(address-base)/PageSize;page<=(address-base+length-1)/PageSize;++page) {
        if(g_pages[page]==Access::None)return false;
        if(need==Access::ReadWrite && g_pages[page]!=Access::ReadWrite)return false;
    }
    return true;
}
static bool Refuse(uint64_t address) {
    const uint64_t base=UserBase();
    if(address<base || address>=base+ArenaPages*PageSize)++g_refusedKernelCopies;
    return false;
}

brook::Process* CurrentProcess() { if(!g_process)ResetProcess();return g_process; }
void ResetProcess() {
    if(!g_process)g_process=static_cast<brook::Process*>(calloc(1,sizeof(brook::Process)));
    else for(uint64_t i=0;i<sizeof(brook::Process);++i)reinterpret_cast<uint8_t*>(g_process)[i]=0;
    for(auto& entry:g_fds)entry=brook::FdEntry{};
    g_process->magic=brook::PROCESS_MAGIC;
    g_process->pid=7;g_process->tgid=7;
    g_process->fds=g_fds;
}

void Check(bool condition,const char* expression,const char* file,int line) {
    if(condition)return;
    ++g_failures;fprintf(stderr,"FAIL %s:%d: %s\n",file,line,expression);
}
unsigned Failures() { return g_failures; }
}

// --- Kernel symbols shared by every TU --------------------------------------
extern "C" void* kmalloc(uint64_t size) { return malloc(size?size:1); }
extern "C" void kfree(void* p) { free(p); }

namespace brook {
using harness::Access;
extern "C" void SerialPuts(const char*) {}
extern "C" void SerialPrintf(const char*,...) {}
extern "C" void SerialPutChar(char) {}
void SerialLock() {}
void SerialUnlock() {}
uint64_t ApicTickCount() { return 1000; }
Process* ProcessCurrent() { return harness::CurrentProcess(); }
void SchedulerKernelRegionEnter(Process*) {}
void SchedulerKernelRegionLeave(Process*) {}
UserAccessScope::UserAccessScope(Process*) {}
UserAccessScope::~UserAccessScope() {}

bool UserBufferReadable(uint64_t address,uint64_t length) {
    return harness::InArena(address,length,Access::Read) || harness::Refuse(address);
}
bool UserBufferWritable(uint64_t address,uint64_t length) {
    return harness::InArena(address,length,Access::ReadWrite) || harness::Refuse(address);
}
uint64_t CopyFromUserBytes(void* destination,uint64_t source,uint64_t length) {
    if(!length || !harness::InArena(source,length,Access::Read)) { if(length)harness::Refuse(source);return 0; }
    __builtin_memcpy(destination,reinterpret_cast<const void*>(source),length);return length;
}
uint64_t CopyToUserBytes(uint64_t destination,const void* source,uint64_t length) {
    if(!length || !harness::InArena(destination,length,Access::ReadWrite)) { if(length)harness::Refuse(destination);return 0; }
    __builtin_memcpy(reinterpret_cast<void*>(destination),source,length);return length;
}
int CopyUserCStringResult(uint64_t address,char* destination,uint32_t capacity) {
    if(!destination || !capacity)return -22;
    for(uint32_t i=0;i<capacity;++i) {
        if(!harness::InArena(address+i,1,Access::Read)) { harness::Refuse(address+i);return -14; }
        destination[i]=*reinterpret_cast<const char*>(address+i);
        if(!destination[i])return 0;
    }
    destination[capacity-1]=0;return -36;
}
bool CopyUserCString(uint64_t address,char* destination,uint32_t capacity) {
    return CopyUserCStringResult(address,destination,capacity)==0;
}

bool StrEq(const char* a,const char* b) {
    while(*a && *b) { if(*a++!=*b++)return false; }
    return *a==*b;
}
// Scheduling and locking: tests never block; a wait returns at once.
bool g_pendingSignals;
bool HasPendingSignals() { return g_pendingSignals; }
bool HasReadInterruptSignal() { return g_pendingSignals; }
void WakeProcess(Process*) {}
void SchedulerBlock(Process*) { g_pendingSignals=true; }
void SchedulerBlockInterruptible(Process*) { g_pendingSignals=true; }
void SchedulerUnblock(Process*) {}
void KMutexInit(KMutex*) {}
void KMutexLock(KMutex*) {}
void KMutexUnlock(KMutex*) {}
FdEntry* FdGetRef(Process* proc,int fd) {
    if(!proc || fd<0 || fd>=static_cast<int>(MAX_FDS) || proc->fds[fd].type==FdType::None)return nullptr;
    ++proc->fds[fd].pinCount;return &proc->fds[fd];
}
void FdPut(Process* proc,int fd) { --proc->fds[fd].pinCount; }
}

int main() {
    RunInputWmTests();
    RunDirOpenTests();
    RunRwTests();
    RunSocketTests();
    if(harness::Failures()) { fprintf(stderr,"%u failure(s)\n",harness::Failures());return 1; }
    printf("test_syscall_user_memory: all passed\n");
    return 0;
}

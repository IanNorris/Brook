#include "process_uts.h"

#include "memory/heap.h"
#include "process.h"
#include "process_user_namespace.h"
#include "spinlock.h"
#include "string.h"

namespace brook {
struct UtsNamespace {
    uint32_t references;
    UserNamespace* owner;
    char hostname[UtsNameBytes];
    char domain[UtsNameBytes];
};
namespace {
IrqSpinLock utsLock;
UtsNamespace initial{0,nullptr,"brook","(none)"};

UtsNamespace* Instance(Process* process)
{
    return process && process->utsNamespace ? process->utsNamespace : &initial;
}

// Drop ownership while the caller holds utsLock. Destruction may release
// a user namespace and therefore happens outside this lock.
UtsNamespace* Drop(UtsNamespace* state)
{
    return state && state!=&initial && !--state->references ? state : nullptr;
}

void Destroy(UtsNamespace* state)
{
    if(!state)return;
    UserNamespaceRelease(state->owner);
    kfree(state);
}
}

void ProcessUtsInherit(Process* target,Process* source)
{
    const auto flags=IrqSpinLockAcquire(&utsLock);
    target->utsNamespace=source->utsNamespace;
    if(target->utsNamespace)++target->utsNamespace->references;
    IrqSpinLockRelease(&utsLock,flags);
}

void ProcessUtsRelease(Process* process)
{
    const auto flags=IrqSpinLockAcquire(&utsLock);
    auto* free=Drop(process->utsNamespace);
    process->utsNamespace=nullptr;
    IrqSpinLockRelease(&utsLock,flags);
    Destroy(free);
}

int ProcessUtsIsolate(Process* process)
{
    auto* copy=static_cast<UtsNamespace*>(kmalloc(sizeof(UtsNamespace)));
    if(!copy)return -12;
    auto* owner=ProcessUserNamespaceAcquire(process);
    const auto flags=IrqSpinLockAcquire(&utsLock);
    auto* previous=Instance(process);
    *copy=*previous;
    copy->owner=owner;
    copy->references=1;
    process->utsNamespace=copy;
    auto* free=Drop(previous);
    IrqSpinLockRelease(&utsLock,flags);
    Destroy(free);
    return 0;
}

void ProcessUtsRead(Process* process,char* hostname,char* domain)
{
    const auto flags=IrqSpinLockAcquire(&utsLock);
    auto* state=Instance(process);
    memcpy(hostname,state->hostname,UtsNameBytes);
    memcpy(domain,state->domain,UtsNameBytes);
    IrqSpinLockRelease(&utsLock,flags);
}

int ProcessUtsWrite(Process* process,bool domain,const char* value,unsigned length)
{
    if(!process)return -3;
    if(length>=UtsNameBytes)return -22;
    auto flags=IrqSpinLockAcquire(&utsLock);
    auto* state=Instance(process);
    auto* owner=state->owner;
    UserNamespaceRetain(owner);
    IrqSpinLockRelease(&utsLock,flags);
    const bool allowed=ProcessCapable(process,owner,CapSysAdmin);
    UserNamespaceRelease(owner);
    if(!allowed)return -1;
    // The caller is the current task. Only that task can unshare its pointer.
    flags=IrqSpinLockAcquire(&utsLock);
    auto* destination=domain ? state->domain : state->hostname;
    memset(destination,0,UtsNameBytes);
    memcpy(destination,value,length);
    IrqSpinLockRelease(&utsLock,flags);
    return 0;
}
}

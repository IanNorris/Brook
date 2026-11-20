#include "process_namespaces.h"

#include "memory/heap.h"
#include "process.h"
#include "process_user_namespace.h"
#include "spinlock.h"
#include "string.h"
#include "sysv_sem.h"

namespace brook {
struct UtsNamespace {
    uint32_t references;
    UserNamespace* owner;
    char hostname[UtsNameBytes];
    char domain[UtsNameBytes];
};
struct IpcNamespace {
    uint32_t references;
    UserNamespace* owner;
};
namespace {
IrqSpinLock namespaceLock;
uint64_t nextNetworkScope=1;
UtsNamespace initial{0,nullptr,"brook","(none)"};

UtsNamespace* Instance(Process* process)
{
    return process && process->utsNamespace ? process->utsNamespace : &initial;
}

// Drop ownership while the caller holds namespaceLock. Destruction may release
// a user namespace and therefore happens outside this lock.
template<class State> State* Drop(State* state)
{
    return state && !--state->references ? state : nullptr;
}

void Destroy(UtsNamespace* state)
{
    if(!state)return;
    UserNamespaceRelease(state->owner);
    kfree(state);
}
}

void ProcessNamespacesInherit(Process* target,Process* source)
{
    const auto flags=IrqSpinLockAcquire(&namespaceLock);
    target->utsNamespace=source->utsNamespace;
    if(target->utsNamespace)++target->utsNamespace->references;
    target->ipcNamespace=source->ipcNamespace;
    if(target->ipcNamespace)++target->ipcNamespace->references;
    target->networkScope=source->networkScope;
    IrqSpinLockRelease(&namespaceLock,flags);
}

static void DestroyIpc(IpcNamespace* state)
{
    if(!state)return;
    SysvSemRetireNamespace(state);
    UserNamespaceRelease(state->owner);
    kfree(state);
}

void ProcessNamespacesRelease(Process* process)
{
    const auto flags=IrqSpinLockAcquire(&namespaceLock);
    auto* oldUts=Drop(process->utsNamespace);
    auto* oldIpc=Drop(process->ipcNamespace);
    process->utsNamespace=nullptr;
    process->ipcNamespace=nullptr;
    IrqSpinLockRelease(&namespaceLock,flags);
    Destroy(oldUts);
    DestroyIpc(oldIpc);
}

int ProcessNamespacesIsolate(Process* process,uint64_t flags)
{
    if(!process || flags&~CloneNewRuntimeNamespaces)return -22;
    uint64_t network=0;
    if(flags&CloneNewNet) {
        const auto irq=IrqSpinLockAcquire(&namespaceLock);
        if(nextNetworkScope!=~uint64_t{0})network=nextNetworkScope++;
        IrqSpinLockRelease(&namespaceLock,irq);
        if(!network)return -28;
    }
    UtsNamespace* uts=nullptr;
    IpcNamespace* ipc=nullptr;
    if(flags&CloneNewUts) {
        uts=static_cast<UtsNamespace*>(kmalloc(sizeof(UtsNamespace)));
        if(!uts)return -12;
    }
    if(flags&CloneNewIpc) {
        ipc=static_cast<IpcNamespace*>(kmalloc(sizeof(IpcNamespace)));
        if(!ipc) {kfree(uts);return -12;}
        ipc->references=1;
        ipc->owner=ProcessUserNamespaceAcquire(process);
        // Finish this task's SEM_UNDO membership in the old domain before
        // publishing the replacement. No allocation or failure follows.
        SysvSemExit(process);
    }
    auto* utsOwner=uts ? ProcessUserNamespaceAcquire(process) : nullptr;
    const auto irq=IrqSpinLockAcquire(&namespaceLock);
    UtsNamespace* oldUts=nullptr;
    IpcNamespace* oldIpc=nullptr;
    if(uts) {
        *uts=*Instance(process);
        uts->references=1;
        uts->owner=utsOwner;
        oldUts=Drop(process->utsNamespace);
        process->utsNamespace=uts;
    }
    if(ipc) {
        oldIpc=Drop(process->ipcNamespace);
        process->ipcNamespace=ipc;
    }
    if(network)process->networkScope=network;
    IrqSpinLockRelease(&namespaceLock,irq);
    Destroy(oldUts);
    DestroyIpc(oldIpc);
    return 0;
}

const IpcNamespace* ProcessIpcScope(Process* process)
{
    const auto irq=IrqSpinLockAcquire(&namespaceLock);
    auto* scope=process ? process->ipcNamespace : nullptr;
    IrqSpinLockRelease(&namespaceLock,irq);
    return scope;
}

UserNamespace* ProcessIpcOwner(const IpcNamespace* scope)
{
    return scope ? scope->owner : nullptr;
}

uint64_t ProcessNetworkScope(Process* process)
{
    const auto irq=IrqSpinLockAcquire(&namespaceLock);
    const uint64_t scope=process ? process->networkScope : 0;
    IrqSpinLockRelease(&namespaceLock,irq);
    return scope;
}

void ProcessUtsRead(Process* process,char* hostname,char* domain)
{
    const auto flags=IrqSpinLockAcquire(&namespaceLock);
    auto* state=Instance(process);
    memcpy(hostname,state->hostname,UtsNameBytes);
    memcpy(domain,state->domain,UtsNameBytes);
    IrqSpinLockRelease(&namespaceLock,flags);
}

int ProcessUtsWrite(Process* process,bool domain,const char* value,unsigned length)
{
    if(!process)return -3;
    if(length>=UtsNameBytes)return -22;
    auto flags=IrqSpinLockAcquire(&namespaceLock);
    auto* state=Instance(process);
    auto* owner=state->owner;
    UserNamespaceRetain(owner);
    IrqSpinLockRelease(&namespaceLock,flags);
    const bool allowed=ProcessCapable(process,owner,CapSysAdmin);
    UserNamespaceRelease(owner);
    if(!allowed)return -1;
    // The caller is the current task. Only that task can unshare its pointer.
    flags=IrqSpinLockAcquire(&namespaceLock);
    auto* destination=domain ? state->domain : state->hostname;
    memset(destination,0,UtsNameBytes);
    memcpy(destination,value,length);
    IrqSpinLockRelease(&namespaceLock,flags);
    return 0;
}
}

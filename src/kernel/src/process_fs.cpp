#include "process_fs.h"
#include "process.h"
#include "spinlock.h"
#include "mount_namespace.h"
namespace brook {
static IrqSpinLock g_fsPublicationLock;
FsContext* ProcessFsAcquire(Process* process)
{
    if(!process)return nullptr;
    const uint64_t irqFlags=IrqSpinLockAcquire(&g_fsPublicationLock);
    FsContext* context=process->fsContext;FsContextRetain(context);
    IrqSpinLockRelease(&g_fsPublicationLock,irqFlags);return context;
}
void ProcessFsReplaceOwned(Process* process,FsContext* owned)
{
    if(!process) { FsContextRelease(owned);return; }
    // This helper replaces cwd/mask state in the task's existing namespace.
    // Only unpublished isolation may change the member token before calling it.
    if(owned && FsContextMountNamespace(owned)!=__atomic_load_n(&process->mountNamespaceMember,__ATOMIC_ACQUIRE))__builtin_trap();
    const uint64_t irqFlags=IrqSpinLockAcquire(&g_fsPublicationLock);
    FsContext* previous=process->fsContext;process->fsContext=owned;
    IrqSpinLockRelease(&g_fsPublicationLock,irqFlags);FsContextRelease(previous);
}
MountNamespace* ProcessFsRetireMounts(Process* process)
{
    if(!process)return nullptr;
    auto* ns=__atomic_exchange_n(&process->mountNamespaceMember,static_cast<MountNamespace*>(nullptr),__ATOMIC_ACQ_REL);
    VfsMountNamespaceLeave(ns,false);return ns;
}
void ProcessFsFinishMountRetirement(MountNamespace* owned)
{
    if(!owned)return;
    VfsMountNamespaceRelease(owned);VfsWakeMountCleanup();
}
void ProcessFsClear(Process* process)
{
    ProcessFsFinishMountRetirement(ProcessFsRetireMounts(process));
    ProcessFsReplaceOwned(process,nullptr);
}
static int ProcessFsAdoptMember(Process* process,FsContext* context)
{
    FsSnapshot snapshot{};int error=FsContextSnapshot(context,&snapshot);
    if(error)return error;
    if(!VfsMountNamespaceJoin(snapshot.mountNamespace)) {FsSnapshotRelease(&snapshot);return -3;}
    if(process->mountNamespaceMember)__builtin_trap(); // unpublished fresh task only
    process->mountNamespaceMember=snapshot.mountNamespace;snapshot.mountNamespace=nullptr;
    FsSnapshotRelease(&snapshot);return 0;
}
int ProcessFsSnapshot(Process* process,FsSnapshot* out)
{
    FsContext* context=ProcessFsAcquire(process);
    int error=FsContextSnapshot(context,out);FsContextRelease(context);return error;
}
FsContext* ProcessFsCopy(Process* process,int* error)
{
    FsContext* context=ProcessFsAcquire(process);
    FsContext* copied=FsContextCopy(context,error);FsContextRelease(context);return copied;
}
int ProcessFsInherit(Process* child,Process* parent,bool share)
{
    FsContext* context=ProcessFsAcquire(parent);if(!context)return -9;
    int error=0;
    if(!share) {
        FsContext* copied=FsContextCopy(context,&error);FsContextRelease(context);context=copied;
    }
    if(error)return error;
    if((error=ProcessFsAdoptMember(child,context))) {FsContextRelease(context);return error;}
    ProcessFsReplaceOwned(child,context);return 0;
}
int ProcessFsIsolateMounts(Process* child)
{
    if(!child)return -22;
    FsSnapshot snapshot{};int error=ProcessFsSnapshot(child,&snapshot);
    if(error)return error;
    MountNamespace* ns=nullptr;VfsPath root{},cwd{};
    error=VfsMountNamespaceCopy(snapshot.mountNamespace,snapshot.root,snapshot.cwd,&ns,&root,&cwd);
    FsContext* context=nullptr;
    if(!error)error=VfsMountNamespaceSetUserOwnerPrepared(ns,child->userNamespace);
    if(!error)context=FsContextCreate(root,cwd,snapshot.mask,&error,ns);
    VfsPathRelease(cwd);VfsPathRelease(root);FsSnapshotRelease(&snapshot);
    if(error) {
        if(ns) {if(!VfsMountNamespaceAbandon(ns))__builtin_trap();VfsMountNamespaceRelease(ns);}
        return error;
    }
    if(!VfsMountNamespaceActivate(ns))__builtin_trap();
    auto* old=__atomic_exchange_n(&child->mountNamespaceMember,ns,__ATOMIC_ACQ_REL);
    VfsMountNamespaceLeave(old,false);
    ProcessFsReplaceOwned(child,context);
    ProcessFsFinishMountRetirement(old);return 0;
}
static void ProcessFsRefreshDisplay(Process* process)
{
    char display[256]{};ProcessFsGetcwd(process,display,sizeof(display));
    const uint64_t irqFlags=IrqSpinLockAcquire(&g_fsPublicationLock);
    for(uint32_t i=0;i<sizeof(display);++i)process->cwdDisplay[i]=display[i];
    IrqSpinLockRelease(&g_fsPublicationLock,irqFlags);
}
void ProcessFsReadDisplay(const Process* process,char* out,uint32_t capacity)
{
    if(!out || !capacity)return;
    const uint64_t irqFlags=IrqSpinLockAcquire(&g_fsPublicationLock);uint32_t i=0;
    if(process)for(;i+1<capacity && i<sizeof(process->cwdDisplay) && process->cwdDisplay[i];++i)out[i]=process->cwdDisplay[i];
    out[i]=0;IrqSpinLockRelease(&g_fsPublicationLock,irqFlags);
}
int ProcessFsInitialize(Process* process,const char* cwd)
{
    if(!process)return -22;
    VfsPath root{},directory{};int error=VfsPathRoot(&root);
    if(!error)error=VfsPathResolve(root,root,cwd,&directory);
    FsContext* context=nullptr;
    if(!error)context=FsContextCreate(root,directory,022,&error);
    VfsPathRelease(directory);VfsPathRelease(root);
    if(error)return error;
    if((error=ProcessFsAdoptMember(process,context))) {FsContextRelease(context);return error;}
    ProcessFsReplaceOwned(process,context);ProcessFsRefreshDisplay(process);return 0;
}
int ProcessFsSetCwd(Process* process,const VfsPath& path)
{
    FsContext* context=ProcessFsAcquire(process);int error=FsContextSetCwd(context,path);
    FsContextRelease(context);if(!error)ProcessFsRefreshDisplay(process);return error;
}
int ProcessFsSetInitialCwd(Process* process,const char* absolute)
{
    if(!absolute || absolute[0]!='/')return -22;
    FsSnapshot snapshot{};int error=ProcessFsSnapshot(process,&snapshot);VfsPath path{};
    if(!error)error=VfsPathResolve(snapshot.root,snapshot.root,absolute,&path);
    if(!error)error=ProcessFsSetCwd(process,path);
    VfsPathRelease(path);FsSnapshotRelease(&snapshot);return error;
}
int ProcessFsGetcwdFromVfsCallback(Process* process,char* out,uint32_t capacity)
{
    FsSnapshot snapshot{};int error=ProcessFsSnapshot(process,&snapshot);
    if(!error)error=VfsPathDirectoryPathFromCallback(snapshot.cwd,out,capacity);
    FsSnapshotRelease(&snapshot);return error;
}
int ProcessFsGetcwd(Process* process,char* out,uint32_t capacity)
{
    FsSnapshot snapshot{};int error=ProcessFsSnapshot(process,&snapshot);
    if(!error)error=VfsPathDirectoryPath(snapshot.cwd,out,capacity);
    FsSnapshotRelease(&snapshot);return error;
}
int ProcessFsMask(Process* process)
{
    FsContext* context=ProcessFsAcquire(process);
    // Kernel-created backend operations use the initial namespace's default
    // creation mask. User processes must always have an installed context.
    int mask=context?FsContextReadMask(context):(!process || process->isKernelThread?022:-9);
    FsContextRelease(context);return mask;
}
int ProcessFsExchangeMask(Process* process,uint16_t mask)
{
    FsContext* context=ProcessFsAcquire(process);int old=FsContextExchangeMask(context,mask);
    FsContextRelease(context);return old;
}
} // namespace brook

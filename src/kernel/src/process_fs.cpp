#include "process_fs.h"
#include "process.h"
#include "spinlock.h"
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
    const uint64_t irqFlags=IrqSpinLockAcquire(&g_fsPublicationLock);
    FsContext* previous=process->fsContext;process->fsContext=owned;
    IrqSpinLockRelease(&g_fsPublicationLock,irqFlags);FsContextRelease(previous);
}
void ProcessFsClear(Process* process) { ProcessFsReplaceOwned(process,nullptr); }
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
    ProcessFsReplaceOwned(child,context);return 0;
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

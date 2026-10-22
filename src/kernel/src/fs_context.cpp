#include "fs_context.h"
#include "memory/heap.h"
#include "spinlock.h"

namespace brook {
struct FsContext {
    SpinLock lock;
    uint32_t references;
    VfsPath root,cwd;
    uint16_t mask;
};
struct FsContextGuard {
    FsContext* context;
    explicit FsContextGuard(FsContext* value):context(value) { SpinLockAcquire(&context->lock); }
    ~FsContextGuard() { SpinLockRelease(&context->lock); }
};
void FsSnapshotRelease(FsSnapshot* snapshot)
{
    if(!snapshot)return;
    FsSnapshot owned=*snapshot;*snapshot={};
    VfsPathRelease(owned.cwd);VfsPathRelease(owned.root);
}
static FsContext* FsAdoptSnapshot(FsSnapshot* owned,int* error)
{
    auto* context=static_cast<FsContext*>(kmalloc(sizeof(FsContext)));
    if(!context) { if(error)*error=-12;return nullptr; }
    *context={};context->references=1;context->root=owned->root;context->cwd=owned->cwd;
    context->mask=owned->mask&0777;*owned={};return context;
}
FsContext* FsContextCreate(const VfsPath& root,const VfsPath& cwd,uint16_t mask,int* error)
{
    int local=0;if(!error)error=&local;*error=0;
    VnodeStat stat{};*error=VfsPathStat(root,&stat);
    if(!*error && !stat.isDir)*error=-20;
    if(*error)return nullptr;
    *error=VfsPathStat(cwd,&stat);
    if(!*error && !stat.isDir)*error=-20;
    if(*error)return nullptr;
    FsSnapshot snapshot{root,cwd,uint16_t(mask&0777)};
    VfsPathRetain(root);VfsPathRetain(cwd);
    FsContext* context=FsAdoptSnapshot(&snapshot,error);FsSnapshotRelease(&snapshot);return context;
}
void FsContextRetain(FsContext* context)
{
    if(!context)return;
    uint32_t count=__atomic_load_n(&context->references,__ATOMIC_RELAXED);
    do { if(!count || count==~uint32_t{0})__builtin_trap(); }
    while(!__atomic_compare_exchange_n(&context->references,&count,count+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
}
void FsContextRelease(FsContext* context)
{
    if(!context)return;
    uint32_t count=__atomic_load_n(&context->references,__ATOMIC_RELAXED);
    do { if(!count)__builtin_trap(); }
    while(!__atomic_compare_exchange_n(&context->references,&count,count-1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED));
    if(count!=1)return;
    FsSnapshot owned{context->root,context->cwd,context->mask};kfree(context);FsSnapshotRelease(&owned);
}
int FsContextSnapshot(FsContext* context,FsSnapshot* out)
{
    if(!out)return -22;
    *out={};if(!context)return -9;
    FsContextGuard held(context);
    *out={context->root,context->cwd,context->mask};VfsPathRetain(out->root);VfsPathRetain(out->cwd);return 0;
}
FsContext* FsContextCopy(FsContext* context,int* error)
{
    int local=0;if(!error)error=&local;
    FsSnapshot snapshot{};*error=FsContextSnapshot(context,&snapshot);if(*error)return nullptr;
    FsContext* copied=FsAdoptSnapshot(&snapshot,error);FsSnapshotRelease(&snapshot);return copied;
}
int FsContextSetCwd(FsContext* context,const VfsPath& cwd)
{
    if(!context)return -9;
    VnodeStat stat{};if(int error=VfsPathStat(cwd,&stat))return error;
    if(!stat.isDir)return -20;
    if(int error=VfsPathAccess(cwd,1))return error;
    VfsPathRetain(cwd);VfsPath previous{};
    { FsContextGuard held(context);previous=context->cwd;context->cwd=cwd; }
    VfsPathRelease(previous);return 0;
}
int FsContextReadMask(FsContext* context)
{
    if(!context)return -9;
    FsContextGuard held(context);return context->mask;
}
int FsContextExchangeMask(FsContext* context,uint16_t mask)
{
    if(!context)return -9;
    FsContextGuard held(context);uint16_t old=context->mask;context->mask=mask&0777;return old;
}
} // namespace brook

#include "fs_context.h"
#include "memory/heap.h"
#include "spinlock.h"
#include "mount_namespace.h"

namespace brook {
struct FsContext {
    SpinLock lock;
    uint32_t references;
    VfsPath root,cwd;
    uint16_t mask;
    MountNamespace* mountNamespace;
    uint64_t revision;
    FsContext* previous;
    FsContext* next;
    bool registered;
};
// Lock order: VFS transaction (when needed), registry, individual context.
// Snapshots join the registry lock so a root-move graph commit and all affected
// contexts are one publication window. No backend callback/free runs in these locks.
static SpinLock g_contextRegistryLock;
static FsContext* g_contexts;
struct FsRegistryGuard {
    FsRegistryGuard() {SpinLockAcquire(&g_contextRegistryLock);}
    ~FsRegistryGuard() {SpinLockRelease(&g_contextRegistryLock);}
};
struct FsContextGuard {
    FsRegistryGuard registry;
    FsContext* context;
    explicit FsContextGuard(FsContext* value):context(value) {SpinLockAcquire(&context->lock);}
    ~FsContextGuard() {SpinLockRelease(&context->lock);}
};
static void FsLinkLocked(FsContext* context) {
    context->next=g_contexts;if(g_contexts)g_contexts->previous=context;
    g_contexts=context;context->registered=true;
}
static void FsPublishPrepared(void* argument) {
    FsRegistryGuard held;FsLinkLocked(static_cast<FsContext*>(argument));
}
void FsSnapshotRelease(FsSnapshot* snapshot)
{
    if(!snapshot)return;
    FsSnapshot owned=*snapshot;*snapshot={};
    VfsPathRelease(owned.cwd);VfsPathRelease(owned.root);
    VfsMountNamespaceRelease(owned.mountNamespace);
}
static FsContext* FsAdoptSnapshot(FsSnapshot* owned,int* error)
{
    auto* context=static_cast<FsContext*>(kmalloc(sizeof(FsContext)));
    if(!context) {if(error)*error=-12;return nullptr;}
    *context={};context->references=1;context->root=owned->root;context->cwd=owned->cwd;
    context->mask=owned->mask&0777;context->mountNamespace=owned->mountNamespace;
    context->revision=1;*owned={};
    // A caller may have obtained its explicit root before a concurrent pivot.
    // Validate the exact current/prepared namespace root under the transaction
    // through registry insertion; stale input never becomes a new context.
    int result=VfsMountNamespacePublishContext(context->mountNamespace,context->root,FsPublishPrepared,context);
    if(result) {if(error)*error=result;FsContextRelease(context);return nullptr;}
    return context;
}
FsContext* FsContextCreate(const VfsPath& root,const VfsPath& cwd,uint16_t mask,int* error,MountNamespace* ns)
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
    if(ns) {VfsMountNamespaceRetain(ns);snapshot.mountNamespace=ns;}
    else snapshot.mountNamespace=VfsMountNamespaceInitial();
    FsContext* context=FsAdoptSnapshot(&snapshot,error);FsSnapshotRelease(&snapshot);return context;
}
void FsContextRetain(FsContext* context)
{
    if(!context)return;
    uint32_t count=__atomic_load_n(&context->references,__ATOMIC_RELAXED);
    do {if(!count || count==~uint32_t{0})__builtin_trap();}
    while(!__atomic_compare_exchange_n(&context->references,&count,count+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
}
MountNamespace* FsContextMountNamespace(FsContext* context) {return context?context->mountNamespace:nullptr;}
void FsContextRelease(FsContext* context)
{
    if(!context)return;
    uint32_t count=__atomic_load_n(&context->references,__ATOMIC_RELAXED);
    do {if(!count)__builtin_trap();}
    while(!__atomic_compare_exchange_n(&context->references,&count,count-1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED));
    if(count!=1)return;
    FsSnapshot owned{};
    {
        FsRegistryGuard held;
        if(context->registered) {
            if(context->previous)context->previous->next=context->next;else g_contexts=context->next;
            if(context->next)context->next->previous=context->previous;
        }
        owned={context->root,context->cwd,context->mask,context->mountNamespace,context->revision};
    }
    kfree(context);FsSnapshotRelease(&owned);
}
int FsContextSnapshot(FsContext* context,FsSnapshot* out)
{
    if(!out)return -22;
    *out={};if(!context)return -9;
    FsContextGuard held(context);
    *out={context->root,context->cwd,context->mask,context->mountNamespace,context->revision};
    VfsPathRetain(out->root);VfsPathRetain(out->cwd);VfsMountNamespaceRetain(out->mountNamespace);return 0;
}
FsContext* FsContextCopy(FsContext* context,int* error)
{
    int local=0;if(!error)error=&local;*error=0;
    if(!context) {*error=-9;return nullptr;}
    auto* copy=static_cast<FsContext*>(kmalloc(sizeof(FsContext)));
    if(!copy) {*error=-12;return nullptr;}
    *copy={};copy->references=1;
    {
        // Copy and registry publication cannot straddle a root move.
        FsContextGuard held(context);
        copy->root=context->root;copy->cwd=context->cwd;copy->mask=context->mask;
        copy->mountNamespace=context->mountNamespace;copy->revision=context->revision;
        VfsPathRetain(copy->root);VfsPathRetain(copy->cwd);VfsMountNamespaceRetain(copy->mountNamespace);
        FsLinkLocked(copy);
    }
    return copy;
}
static int FsSetCwd(FsContext* context,const VfsPath& cwd,const FsSnapshot* expected)
{
    if(!context)return -9;
    VnodeStat stat{};if(int error=VfsPathStat(cwd,&stat))return error;
    if(!stat.isDir)return -20;
    if(int error=VfsPathAccess(cwd,1))return error;
    VfsPathRetain(cwd);VfsPath previous{};int error=0;
    {
        FsContextGuard held(context);
        if(expected && (context->revision!=expected->revision || context->mountNamespace!=expected->mountNamespace || !VfsPathSame(context->root,expected->root)))error=-11;
        else if(!VfsPathSame(context->cwd,cwd)) {
            if(context->revision==~uint64_t{0})error=-75;
            else {previous=context->cwd;context->cwd=cwd;++context->revision;}
        }
    }
    if(previous.object)VfsPathRelease(previous);else VfsPathRelease(cwd);
    return error;
}
int FsContextSetCwd(FsContext* context,const VfsPath& cwd) {return FsSetCwd(context,cwd,nullptr);}
int FsContextSetResolvedCwd(FsContext* context,const VfsPath& cwd,const FsSnapshot& expected) {return FsSetCwd(context,cwd,&expected);}
int FsContextCommitRootMove(const VfsPath& oldRoot,const VfsPath& newRoot,void (*publishGraph)(void*),void* argument)
{
    if(!oldRoot.object || !newRoot.object || !publishGraph)return -22;
    uint64_t replaced=0;
    {
        FsRegistryGuard registry;
        // Fail before graph publication if a revision cannot advance.
        for(auto* context=g_contexts;context;context=context->next) {
            if((VfsPathSame(context->root,oldRoot) || VfsPathSame(context->cwd,oldRoot)) && context->revision==~uint64_t{0})return -75;
        }
        publishGraph(argument); // prevalidated topology stores, never frees
        for(auto* context=g_contexts;context;context=context->next) {
            SpinLockAcquire(&context->lock);bool changed=false;
            VfsPath* paths[2]={&context->root,&context->cwd};
            for(auto* path:paths)if(VfsPathSame(*path,oldRoot)) {
                VfsPathRetain(newRoot);*path=newRoot;++replaced;changed=true;
            }
            if(changed)++context->revision;
            SpinLockRelease(&context->lock);
        }
    }
    while(replaced--)VfsPathRelease(oldRoot);
    return 0;
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

// Included within namespace brook after retained VFS operations.
static void VfsCollectRetiredMountsLocked()
{
    bool progress;
    do {
        progress=false;auto** entry=&g_retiredMounts;
        while(*entry) {
            auto* mount=*entry;
            if((__atomic_load_n(&mount->referenceState,__ATOMIC_ACQUIRE)&~MOUNT_DETACHED)!=0) { entry=&mount->next;continue; }
            *entry=mount->next;
            VfsObjectRelease(mount->rootObject);
            VfsBackingDropLocked(mount->backing,true);
            kfree(mount);progress=true;
        }
    } while(progress);
}
static void VfsDetachMountLocked(MountEntry* mount)
{
    if(!mount->used)__builtin_trap();
    g_mounts.Remove(mount);
    __atomic_fetch_or(&mount->referenceState,MOUNT_DETACHED,__ATOMIC_RELEASE);
    mount->owner=nullptr;mount->linked=false;
    VfsPath covered=mount->covered;mount->covered={};
    mount->next=g_retiredMounts;g_retiredMounts=mount;
    VfsPathRelease(covered);VfsMountDrop(mount); // publication lease
}
static void VfsRetireNamespaceLocked(MountNamespace* ns)
{
    if(ns->initial)__builtin_trap();
    for(auto* mount=g_mounts.head;mount;) {
        auto* next=mount->next;
        if(mount->owner==ns)VfsDetachMountLocked(mount);
        mount=next;
    }
    ns->root=nullptr;
    VfsCollectRetiredMountsLocked();
    VfsMountNamespaceRelease(ns); // registry's storage lease
}
static void VfsQueueNamespaceRetirement(MountNamespace* ns,bool wake)
{
    auto flags=IrqSpinLockAcquire(&g_namespaceRetirementLock);
    ns->retireNext=g_namespaceRetirements;g_namespaceRetirements=ns;
    IrqSpinLockRelease(&g_namespaceRetirementLock,flags);
    VfsRequestMountCleanup(wake);
}
MountNamespace* VfsMountNamespaceInitial()
{
    g_initialNamespace.life.RetainStorage();return &g_initialNamespace;
}
void VfsMountNamespaceRetain(MountNamespace* ns) { if(ns)ns->life.RetainStorage(); }
void VfsMountNamespaceRelease(MountNamespace* ns)
{
    if(ns && ns->life.ReleaseStorage()) {
        if(ns->initial || ns->root)__builtin_trap();
        UserNamespaceRelease(ns->userOwner);ns->~MountNamespace();kfree(ns);
    }
}
UserNamespace* VfsMountNamespaceUserOwnerAcquire(MountNamespace* ns) {
    if(!ns)return nullptr;UserNamespaceRetain(ns->userOwner);return ns->userOwner;
}
int VfsMountNamespaceSetUserOwnerPrepared(MountNamespace* ns,UserNamespace* owner) {
    if(!ns || ns->initial || ns->life.Published() || ns->life.IsRetired())return -22;
    if(ns->userOwner==owner)return 0;
    FileMutationGuard transaction;
    UserNamespaceRetain(owner);auto* old=ns->userOwner;ns->userOwner=owner;
    for(auto& mount:g_mounts)if(mount.owner==ns)mount.lockedInherited=true;
    UserNamespaceRelease(old);return 0;
}
uint64_t VfsMountNamespaceIdentity(MountNamespace* ns) { return ns?ns->identity:0; }
bool VfsMountNamespaceHandleAcquire(MountNamespace* ns)
{
    if(!ns)return false;
    VfsMountNamespaceRetain(ns);
    if(!ns->life.TryJoin()) {VfsMountNamespaceRelease(ns);return false;}
    return true;
}
void VfsMountNamespaceHandleRelease(MountNamespace* ns)
{
    if(!ns)return;
    VfsMountNamespaceLeave(ns);VfsMountNamespaceRelease(ns);
}
bool VfsMountNamespaceActivate(MountNamespace* ns) { return ns && ns->life.ActivatePrepared(); }
bool VfsMountNamespaceJoin(MountNamespace* ns) { return ns && ns->life.TryJoin(); }
bool VfsMountNamespaceAbandon(MountNamespace* ns,bool wake)
{
    if(!ns || !ns->life.AbandonPrepared())return false;
    VfsQueueNamespaceRetirement(ns,wake);return true;
}
void VfsMountNamespaceLeave(MountNamespace* ns,bool wake)
{
    if(ns && ns->life.LeaveMember())VfsQueueNamespaceRetirement(ns,wake);
}
void VfsDrainMountCleanup()
{
    __atomic_store_n(&g_mountCleanupPending,false,__ATOMIC_RELEASE);
    auto flags=IrqSpinLockAcquire(&g_namespaceRetirementLock);
    auto* retired=g_namespaceRetirements;g_namespaceRetirements=nullptr;
    IrqSpinLockRelease(&g_namespaceRetirementLock,flags);
    FileMutationGuard transaction;
    while(retired) {
        auto* next=retired->retireNext;retired->retireNext=nullptr;
        VfsRetireNamespaceLocked(retired);retired=next;
    }
    VfsCollectRetiredMountsLocked();
}
struct VfsNamespaceCloneMap {
    struct Attachment { Attachment* next;MountEntry* oldMount;MountEntry* newMount; };
    struct Object { Object* next;VfsObject* oldObject;VfsObject* newObject; };
    Attachment* attachments=nullptr;Object* objects=nullptr;
    ~VfsNamespaceCloneMap() { Clear(); }
    void Clear() {
        while(objects) {auto* next=objects->next;VfsObjectRelease(objects->newObject);kfree(objects);objects=next;}
        while(attachments) {auto* next=attachments->next;kfree(attachments);attachments=next;}
    }
    MountEntry* Find(MountEntry* source) const {
        for(auto* entry=attachments;entry;entry=entry->next)if(entry->oldMount==source)return entry->newMount;
        return nullptr;
    }
    int CloneObject(VfsObject* source,VfsObject** out,uint32_t depth=0) {
        *out=nullptr;if(!source)return -9;
        if(source->ops!=&g_virtualDirectoryOps) {VfsObjectRetain(source);*out=source;return 0;}
        if(depth>=256)return -40;
        auto* directory=static_cast<VfsVirtualDirectory*>(source->priv);
        if(!Find(static_cast<MountEntry*>(directory->parent.attachment))) {VfsObjectRetain(source);*out=source;return 0;}
        for(auto* entry=objects;entry;entry=entry->next)if(entry->oldObject==source) {
            VfsObjectRetain(entry->newObject);*out=entry->newObject;return 0;
        }
        auto* memo=static_cast<Object*>(kmalloc(sizeof(Object)));if(!memo)return -12;
        VfsPath parent{},created{};int error=ClonePath(directory->parent,&parent,depth+1);
        if(!error)error=VfsMakeVirtualLocked(parent,directory->name,&created);
        VfsPathRelease(parent);
        if(error) {kfree(memo);return error;}
        memo->oldObject=source;memo->newObject=created.object;VfsObjectRetain(memo->newObject);
        VfsPathRelease(created);memo->next=objects;objects=memo;
        VfsObjectRetain(memo->newObject);*out=memo->newObject;return 0;
    }
    int ClonePath(const VfsPath& source,VfsPath* out,uint32_t depth=0) {
        *out={};if(!source.object)return 0;
        auto* mapped=Find(static_cast<MountEntry*>(source.attachment));
        if(!mapped) {*out=source;VfsPathRetain(*out);return 0;} // deliberate outside capability
        VfsObject* object=nullptr;int error=CloneObject(source.object,&object,depth);
        if(error)return error;
        VfsPathHoldMount(mapped);*out={mapped,object};return 0;
    }
};
int VfsMountNamespaceCopy(MountNamespace* source,const VfsPath& root,const VfsPath& cwd,
                          MountNamespace** out,VfsPath* newRoot,VfsPath* newCwd)
{
    if(!out || !newRoot || !newCwd || !source)return -22;
    *out=nullptr;*newRoot={};*newCwd={};
    FileMutationGuard transaction;
    if(!source->life.Published() || !source->root)return -3;
    if(!root.object || !root.attachment || !cwd.object || !cwd.attachment)return -9;
    VnodeStat stat{};int validation=VfsObjectStatLocked(root.object,&stat);
    if(validation)return validation;if(!stat.isDir)return -20;
    validation=VfsObjectStatLocked(cwd.object,&stat);
    if(validation)return validation;if(!stat.isDir)return -20;
    void* memory=kmalloc(sizeof(MountNamespace));if(!memory)return -12;
    auto* ns=::new(memory)MountNamespace();
    static uint64_t nextIdentity=2;
    if(nextIdentity==~uint64_t{0})__builtin_trap();
    ns->identity=nextIdentity++;
    ns->userOwner=source->userOwner;UserNamespaceRetain(ns->userOwner);ns->life.RetainStorage(); // registry lease
    VfsNamespaceCloneMap map;int error=0;
    for(auto* original=g_mounts.head;original;original=original->next) {
        if(original->owner!=source || !VfsMountPublished(*original))continue;
        auto* memo=static_cast<VfsNamespaceCloneMap::Attachment*>(kmalloc(sizeof(VfsNamespaceCloneMap::Attachment)));
        if(!memo) {error=-12;break;}
        auto* mount=static_cast<MountEntry*>(kmalloc(sizeof(MountEntry)));
        if(!mount) {kfree(memo);error=-12;break;}
        *mount={};mount->owner=ns;mount->referenceState=1;mount->backing=original->backing;
        mount->backing->RetainLocked();mount->used=true;mount->linked=original->linked;mount->lockedInherited=original->lockedInherited;
        mount->publication=++g_mountPublication;mount->rootIdentity=original->rootIdentity;
        mount->rootObjectOps=original->rootObjectOps;mount->rootIdentityKnown=original->rootIdentityKnown;
        StrCopy(mount->mountPoint,original->mountPoint,sizeof(mount->mountPoint));
        StrCopy(mount->fsType,original->fsType,sizeof(mount->fsType));
        memo->oldMount=original;memo->newMount=mount;memo->next=map.attachments;map.attachments=memo;
        g_mounts.Publish(mount); // prepared namespace is not searchable yet
    }
    if(!error)for(auto* entry=map.attachments;entry;entry=entry->next) {
        error=map.ClonePath(entry->oldMount->covered,&entry->newMount->covered);
        if(!error && entry->oldMount->rootObject) {
            error=map.CloneObject(entry->oldMount->rootObject,&entry->newMount->rootObject);
            if(!error) {entry->newMount->rootIdentity=entry->newMount->rootObject->identity;entry->newMount->rootObjectOps=entry->newMount->rootObject->ops;}
        }
        if(error)break;
    }
    if(!error) {ns->root=map.Find(source->root);if(!ns->root)error=-5;}
    if(!error)error=map.ClonePath(root,newRoot);
    if(!error)error=map.ClonePath(cwd,newCwd);
    if(!error && !source->life.Published())error=-3;
    map.Clear();
    if(error) {
        VfsPathRelease(*newCwd);*newCwd={};VfsPathRelease(*newRoot);*newRoot={};
        if(!ns->life.AbandonPrepared())__builtin_trap();
        VfsRetireNamespaceLocked(ns);VfsMountNamespaceRelease(ns);return error;
    }
    *out=ns;return 0;
}
int VfsMountNamespaceBind(MountNamespace* ns,const VfsPath& source,const VfsPath& target)
{
    if(!ns || !source.object || !source.attachment || !target.object || !target.attachment)return -22;
    FileMutationGuard transaction;
    if(!ns->life.Published())return -3;
    if(VfsVirtual(source))return -95; // synthetic source roots need explicit legacy reporting support
    if(ns->initial && !source.object->ops->directory_path)return -95;
    auto* parent=static_cast<MountEntry*>(target.attachment);
    if(parent->owner!=ns || !VfsMountPublished(*parent))return -22;
    if(parent==ns->root && target.object->identity==parent->rootIdentity && target.object->ops==parent->rootObjectOps)return -95;
    VnodeStat stat{};int error=VfsObjectStatLocked(source.object,&stat);
    if(error)return error;if(!stat.isDir)return -20;
    if((error=VfsObjectStatLocked(target.object,&stat)))return error;if(!stat.isDir)return -20;
    auto* mount=static_cast<MountEntry*>(kmalloc(sizeof(MountEntry)));if(!mount)return -12;
    *mount={};error=VfsGlobalDirectoryLocked(target,mount->mountPoint,sizeof(mount->mountPoint));
    if(error) {kfree(mount);return error;}
    mount->owner=ns;mount->referenceState=1;mount->backing=static_cast<MountEntry*>(source.attachment)->backing;
    mount->backing->RetainLocked();mount->rootObject=source.object;VfsObjectRetain(source.object);
    mount->rootIdentity=source.object->identity;mount->rootObjectOps=source.object->ops;mount->rootIdentityKnown=true;
    mount->covered=target;VfsPathRetain(target);mount->linked=true;mount->used=true;mount->publication=++g_mountPublication;
    StrCopy(mount->fsType,static_cast<MountEntry*>(source.attachment)->fsType,sizeof(mount->fsType));
    g_mounts.Publish(mount);return 0;
}
int VfsMountNamespaceDetach(MountNamespace* ns,const VfsPath& target)
{
    if(!ns || !target.object || !target.attachment)return -22;
    FileMutationGuard transaction;
    if(!ns->life.Published())return -3;
    auto* selected=static_cast<MountEntry*>(target.attachment);
    if(!VfsMountPublished(*selected) || selected->owner!=ns ||
       target.object->identity!=selected->rootIdentity || target.object->ops!=selected->rootObjectOps)return -22;
    if(selected==ns->root)return -95;
    if(selected->lockedInherited)return -22;
    // Pre-mark the subtree before changing parent edges, so recursive detach
    // cannot lose descendants when a parent is unlinked first.
    for(auto& mount:g_mounts)if(mount.owner==ns) {
        auto* ancestor=&mount;
        for(uint32_t depth=0;ancestor && depth<VFS_MAX_MOUNT_DEPTH;++depth) {
            if(ancestor==selected) {mount.detachSelected=true;break;}
            ancestor=static_cast<MountEntry*>(ancestor->covered.attachment);
        }
    }
    for(auto* mount=g_mounts.head;mount;) {
        auto* next=mount->next;if(mount->detachSelected)VfsDetachMountLocked(mount);mount=next;
    }
    VfsCollectRetiredMountsLocked();return 0;
}

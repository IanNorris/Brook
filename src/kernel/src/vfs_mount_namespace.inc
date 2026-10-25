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
    VfsPath coveredParent=mount->coveredParent;mount->coveredParent={};
    mount->next=g_retiredMounts;g_retiredMounts=mount;
    VfsPathRelease(coveredParent);VfsPathRelease(covered);VfsMountDrop(mount); // publication lease
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
        mount->rootDevice=original->rootDevice;mount->attributes=original->attributes;mount->rootFileUnlinked=original->rootFileUnlinked;
        StrCopy(mount->physicalRoot,original->physicalRoot,sizeof(mount->physicalRoot));
        StrCopy(mount->coveredName,original->coveredName,sizeof(mount->coveredName));
        StrCopy(mount->mountPoint,original->mountPoint,sizeof(mount->mountPoint));
        StrCopy(mount->fsType,original->fsType,sizeof(mount->fsType));
        memo->oldMount=original;memo->newMount=mount;memo->next=map.attachments;map.attachments=memo;
        g_mounts.Publish(mount); // prepared namespace is not searchable yet
    }
    if(!error)for(auto* entry=map.attachments;entry;entry=entry->next) {
        error=map.ClonePath(entry->oldMount->covered,&entry->newMount->covered);
        if(!error)error=map.ClonePath(entry->oldMount->coveredParent,&entry->newMount->coveredParent);
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
void VfsMountInfoFree(VfsMountInfo* rows) {kfree(rows);}
int VfsMountNamespaceInfo(MountNamespace* ns,const VfsPath& root,VfsMountInfo** out,uint32_t* count)
{
    if(!out || !count)return -22;*out=nullptr;*count=0;
    if(!ns || !root.object || !root.attachment)return -22;
    constexpr uint32_t MaxRecords=4096;
    for(unsigned attempt=0;attempt<4;++attempt) {
        uint32_t capacity=VfsMountNamespaceCount(ns);
        if(!capacity)return -3;if(capacity>MaxRecords)return -75;
        auto* rows=static_cast<VfsMountInfo*>(kmalloc(sizeof(VfsMountInfo)*capacity));
        if(!rows)return -12;
        for(uint32_t i=0;i<capacity;++i)rows[i]={};
        uint32_t used=0;int error=0;bool grew=false;
        {
            MountViewGuard view;
            // Logical retirement can change Published without viewLock. Sample
            // it once; registry removal/graph changes cannot occur in this window.
            if(!ns->life.Published() || !ns->root)error=-3;
            else if(root.attachment!=ns->root || root.object->identity!=ns->root->rootIdentity || root.object->ops!=ns->root->rootObjectOps)error=-11;
            if(!error)for(const auto& mount:g_mounts) {
                if(!mount.used || mount.owner!=ns)continue;
                if(used==capacity) {grew=true;break;}
                auto* parent=static_cast<MountEntry*>(mount.covered.attachment);
                if(!mount.rootIdentityKnown || !mount.physicalRoot[0] || (&mount!=ns->root && !parent)) {error=-95;break;}
                if(parent && (!parent->used || parent->owner!=ns)) {error=-5;break;}
                const uint64_t id=mount.publication,parentId=parent?parent->publication:0;
                // The shipped consumer uses signed int max_id + 1 indexing.
                if(!id || id>0x7ffffffeULL || parentId>0x7ffffffeULL) {error=-75;break;}
                auto& row=rows[used++];row.id=id;row.parent=parentId;
                row.device=mount.rootDevice;row.attributes=mount.attributes;
                StrCopy(row.root,mount.physicalRoot,sizeof(row.root));
                if(mount.rootFileUnlinked) {
                    constexpr const char* suffix=" (deleted)";uint32_t length=StrLen(row.root);
                    if(length+StrLen(suffix)>=sizeof(row.root)) {error=-36;break;}
                    StrCopy(row.root+length,suffix,sizeof(row.root)-length);
                }
                StrCopy(row.mountPoint,mount.mountPoint,sizeof(row.mountPoint));
                StrCopy(row.fsType,mount.fsType,sizeof(row.fsType));
            }
        }
        if(error) {kfree(rows);return error;}
        if(grew) {kfree(rows);continue;}
        // Consumers resolve equal-path stacked roots using their last row.
        // Build parent indices once, then emit parent-before-child in O(n^2),
        // independently of clone publication order. All work is outside viewLock.
        struct Parent {uint32_t index;bool emitted;};
        auto* parents=static_cast<Parent*>(kmalloc(sizeof(Parent)*used));
        auto* ordered=static_cast<VfsMountInfo*>(kmalloc(sizeof(VfsMountInfo)*used));
        if(!parents || !ordered) {kfree(parents);kfree(ordered);kfree(rows);return -12;}
        for(uint32_t i=0;i<used;++i) {
            parents[i]={used,false};
            for(uint32_t j=0;j<used;++j) {
                if(i!=j && rows[i].id==rows[j].id)error=-5;
                if(rows[i].parent && rows[j].id==rows[i].parent)parents[i].index=j;
            }
            if(rows[i].parent && parents[i].index==used)error=-5;
        }
        uint32_t emitted=0;
        while(!error && emitted<used) {
            uint32_t before=emitted;
            for(uint32_t i=0;i<used;++i) {
                if(parents[i].emitted)continue;
                if(parents[i].index!=used && !parents[parents[i].index].emitted)continue;
                ordered[emitted++]=rows[i];parents[i].emitted=true;
            }
            if(before==emitted)error=-5;
        }
        kfree(parents);kfree(rows);
        if(error) {kfree(ordered);return error;}
        *out=ordered;*count=used;return 0;
    }
    return -11;
}
// Mount-operation targets follow covering mounts even for a retained fd or dot.
// This does not change ordinary path traversal through those capabilities.
struct VfsMountTargetLease {
    VfsPath value{};
    ~VfsMountTargetLease() {VfsPathRelease(value);}
    int Follow(const VfsPath& input) {value=input;VfsPathRetain(value);return VfsEnterMountsLocked(&value);}
};
int VfsMountNamespacePublishContext(MountNamespace* ns,const VfsPath& root,void (*publish)(void*),void* argument)
{
    if(!ns || !root.object || !root.attachment || !publish)return -22;
    FileMutationGuard transaction;
    if(ns->life.IsRetired() || !ns->root)return -3;
    if(root.attachment!=ns->root || root.object->identity!=ns->root->rootIdentity || root.object->ops!=ns->root->rootObjectOps)return -11;
    publish(argument);return 0;
}
int VfsMountNamespaceMountFresh(MountNamespace* ns,const VfsPath& inputTarget,
    const char* fsName,const VfsFsOps* ops,void* privateData,uint8_t pdrv,uint64_t attributes)
{
    if(!ns || !inputTarget.object || !inputTarget.attachment || !fsName || !ops || !privateData)return -22;
    FileMutationGuard transaction;
    if(!ns->life.Published())return -3;
    VfsMountTargetLease lease;if(int error=lease.Follow(inputTarget))return error;
    const auto& target=lease.value;
    if(attributes && (attributes!=6 || !StrEq(fsName,"tmpfs")))return -95;
    // Registered tables/callbacks are permanently provider-pinned. Unregistered
    // or mismatched tables cannot become a backing through this entry point.
    if(FindFsDriver(fsName)!=ops || !ops->root_object || !ops->unmount)return -95;
    if(StrLen(fsName)>=sizeof(MountEntry::fsType))return -36;
    auto* parent=static_cast<MountEntry*>(target.attachment);
    if(parent->owner!=ns || !VfsMountPublished(*parent))return -22;
    if(parent==ns->root && target.object->identity==parent->rootIdentity && target.object->ops==parent->rootObjectOps)return -95;
    VnodeStat stat{};int error=VfsObjectStatLocked(target.object,&stat);
    if(error)return error;if(!stat.isDir)return -20;
    if(g_mounts.count==~uint32_t{0})return -12;
    auto* mount=static_cast<MountEntry*>(kmalloc(sizeof(MountEntry)));if(!mount)return -12;
    *mount={};error=VfsGlobalDirectoryLocked(target,mount->mountPoint,sizeof(mount->mountPoint));
    auto* backing=error?nullptr:static_cast<MountBacking*>(kmalloc(sizeof(MountBacking)));
    if(!error && !backing)error=-12;
    VfsObject* root=nullptr;
    if(!error)error=ops->root_object(privateData,pdrv,&root);
    if(!error && (!root || !root->ops || !root->ops->stat))error=-5;
    if(!error)error=root->ops->stat(root,&stat);
    if(!error && !stat.isDir)error=-20;
    if(error) {
        // Preparation has not consumed the caller's instance. Release every
        // temporary object before returning it, never invoke unmount here.
        VfsObjectRelease(root);kfree(backing);kfree(mount);return error;
    }
    *backing={};backing->fsOps=ops;backing->mountPriv=privateData;backing->pdrv=pdrv;backing->references=1;backing->independent=true;
    mount->owner=ns;mount->referenceState=1;mount->backing=backing;
    mount->rootObject=root;mount->rootIdentity=root->identity;mount->rootObjectOps=root->ops;mount->rootIdentityKnown=true;
    mount->rootDevice=stat.dev;mount->attributes=attributes;StrCopy(mount->physicalRoot,"/",sizeof(mount->physicalRoot));
    mount->covered=target;VfsPathRetain(target);mount->used=true;mount->linked=true;
    mount->publication=++g_mountPublication;StrCopy(mount->fsType,fsName,sizeof(mount->fsType));
    backing->next=g_backings;g_backings=backing;g_mounts.Publish(mount);return 0;
}
// Test exact retained ancestry, never reporting-label prefixes. The first edge
// on the source attachment determines whether the entire child mount belongs
// to a recursive bind of a partial directory on that attachment.
static int VfsBindContainsLocked(const VfsPath& source,const VfsPath& covered,bool* included)
{
    *included=false;if(!covered.object || !covered.attachment)return 0;
    VfsPath current=covered;VfsPathRetain(current);
    int error=0;VfsRetainedProvider provider;
    for(uint32_t depth=0;depth<VFS_MAX_MOUNT_DEPTH;++depth) {
        auto* mount=static_cast<MountEntry*>(current.attachment);
        if(current.attachment!=source.attachment) {
            if(!mount->linked || !mount->covered.object)break;
            VfsPath next=mount->covered;VfsPathRetain(next);VfsPathRelease(current);current=next;
            continue;
        }
        if(VfsPathSame(current,source)) {*included=true;break;}
        if(current.object->identity==mount->rootIdentity && current.object->ops==mount->rootObjectOps)break;
        VfsPath next{};error=provider.Parent(current,source,next);
        if(error)break;
        if(VfsPathSame(current,next)) {VfsPathRelease(next);break;}
        VfsPathRelease(current);current=next;
        if(depth+1==VFS_MAX_MOUNT_DEPTH)error=-40;
    }
    VfsPathRelease(current);return error;
}
static int VfsMountNamespaceRecursiveBindLocked(MountNamespace* ns,const VfsPath& source,const VfsPath& target)
{
    auto* sourceMount=static_cast<MountEntry*>(source.attachment);
    if(!VfsMountPublished(*sourceMount))return -22;
    char sourceLabel[512];
    if(int error=VfsGlobalDirectoryLocked(source,sourceLabel,sizeof(sourceLabel)))return error;
    VfsNamespaceCloneMap map;
    auto prepare=[&](MountEntry* original,MountEntry** out)->int {
        *out=nullptr;
        auto* memo=static_cast<VfsNamespaceCloneMap::Attachment*>(kmalloc(sizeof(VfsNamespaceCloneMap::Attachment)));
        if(!memo)return -12;
        auto* mount=static_cast<MountEntry*>(kmalloc(sizeof(MountEntry)));
        if(!mount) {kfree(memo);return -12;}
        *mount={};mount->owner=ns;mount->referenceState=1;mount->linked=true;
        mount->backing=original->backing;mount->backing->RetainLocked();
        mount->lockedInherited=original->lockedInherited;
        mount->rootIdentity=original->rootIdentity;mount->rootObjectOps=original->rootObjectOps;
        mount->rootIdentityKnown=original->rootIdentityKnown;
        mount->rootDevice=original->rootDevice;mount->attributes=original->attributes;mount->rootFileUnlinked=original->rootFileUnlinked;
        StrCopy(mount->physicalRoot,original->physicalRoot,sizeof(mount->physicalRoot));
        StrCopy(mount->coveredName,original->coveredName,sizeof(mount->coveredName));
        StrCopy(mount->fsType,original->fsType,sizeof(mount->fsType));
        memo->oldMount=original;memo->newMount=mount;memo->next=map.attachments;map.attachments=memo;
        *out=mount;return 0;
    };
    if(g_mounts.count==~uint32_t{0})return -12;
    MountEntry* graft=nullptr;int error=prepare(sourceMount,&graft);
    uint32_t count=error?0:1;
    if(!error) {
        graft->lockedInherited=false; // new bind root; copied descendants keep restrictions
        graft->rootObject=source.object;VfsObjectRetain(source.object);
        graft->rootIdentity=source.object->identity;graft->rootObjectOps=source.object->ops;graft->rootIdentityKnown=true;
        graft->covered=target;VfsPathRetain(target);
        error=VfsGlobalDirectoryLocked(target,graft->mountPoint,sizeof(graft->mountPoint));
        if(!error)error=VfsBackendDirectoryLocked(source,graft->physicalRoot,sizeof(graft->physicalRoot));
    }
    // Freeze the complete pre-operation set. Nothing is added to g_mounts until
    // every edge, virtual ancestor and reporting label has been prepared.
    if(!error)for(auto* original=g_mounts.head;original;original=original->next) {
        if(!VfsMountPublished(*original) || original->owner!=sourceMount->owner || original==sourceMount || !original->linked)continue;
        bool included=false;error=VfsBindContainsLocked(source,original->coveredParent.object?original->coveredParent:original->covered,&included);
        if(error)break;if(!included)continue;
        if(count>=VFS_MAX_MOUNT_DEPTH || count>=~uint32_t{0}-g_mounts.count) {error=-12;break;}
        MountEntry* mount=nullptr;error=prepare(original,&mount);if(error)break;++count;
    }
    if(!error)for(auto* entry=map.attachments;entry;entry=entry->next) {
        if(entry->newMount==graft)continue;
        error=map.ClonePath(entry->oldMount->covered,&entry->newMount->covered);
        if(!error)error=map.ClonePath(entry->oldMount->coveredParent,&entry->newMount->coveredParent);
        if(!error && entry->oldMount->rootObject) {
            error=map.CloneObject(entry->oldMount->rootObject,&entry->newMount->rootObject);
            if(!error) {entry->newMount->rootIdentity=entry->newMount->rootObject->identity;entry->newMount->rootObjectOps=entry->newMount->rootObject->ops;}
        }
        if(error)break;
    }
    if(!error)for(auto* entry=map.attachments;entry;entry=entry->next) {
        auto* mount=entry->newMount;
        if(mount!=graft) {
            // Reporting alone uses names. New virtual ancestors are deliberately
            // unpublished, so derive their labels from the frozen old tree.
            char oldLabel[512];
            if(entry->oldMount->coveredParent.object) {
                error=VfsGlobalDirectoryLocked(entry->oldMount->coveredParent,oldLabel,sizeof(oldLabel));
                if(!error)error=VfsAppendLeaf(oldLabel,sizeof(oldLabel),entry->oldMount->coveredName);
            } else error=VfsGlobalDirectoryLocked(entry->oldMount->covered,oldLabel,sizeof(oldLabel));
            if(!error) {
                const bool sourceRoot=StrEq(sourceLabel,"/");
                if(!sourceRoot && !VfsComponentPrefix(sourceLabel,oldLabel))error=-2;
                else {
                    const char* suffix=oldLabel+(sourceRoot?0:StrLen(sourceLabel));
                    uint32_t prefix=StrEq(graft->mountPoint,"/")?0:StrLen(graft->mountPoint);
                    if(prefix+StrLen(suffix)>=sizeof(mount->mountPoint))error=-36;
                    else {for(uint32_t i=0;i<prefix;++i)mount->mountPoint[i]=graft->mountPoint[i];StrCopy(mount->mountPoint+prefix,suffix,sizeof(mount->mountPoint)-prefix);}
                }
            }
        }
        if(error)break;
        auto* ancestor=mount;uint32_t depth=0;
        for(;ancestor && depth<VFS_MAX_MOUNT_DEPTH;++depth)ancestor=static_cast<MountEntry*>(ancestor->covered.attachment);
        if(ancestor) {error=-40;break;}
    }
    if(!error && (!ns->life.Published() || !VfsMountPublished(*sourceMount)))error=-3;
    if(error) {
        // Release all cross-edges before freeing any attachment. The memoized
        // virtual objects may own further parent paths into this prepared set.
        for(auto* entry=map.attachments;entry;entry=entry->next) {
            auto* mount=entry->newMount;VfsPathRelease(mount->covered);mount->covered={};
            VfsPathRelease(mount->coveredParent);mount->coveredParent={};
            VfsObjectRelease(mount->rootObject);mount->rootObject=nullptr;
        }
        while(map.objects) {auto* memo=map.objects;map.objects=memo->next;VfsObjectRelease(memo->newObject);kfree(memo);}
        for(auto* entry=map.attachments;entry;entry=entry->next) {
            auto* mount=entry->newMount;if(mount->referenceState!=1)__builtin_trap();
            VfsBackingDropLocked(mount->backing,true);kfree(mount);
        }
        return error;
    }
    // Normal walkers hold the outer transaction; metadata readers hold only
    // viewLock. Publish the entire graft in one window for both classes.
    auto flags=IrqSpinLockAcquire(&g_mountViewLock);
    for(auto* entry=map.attachments;entry;entry=entry->next) {
        auto* mount=entry->newMount;mount->used=true;mount->linked=true;
        mount->publication=++g_mountPublication;mount->next=g_mounts.head;g_mounts.head=mount;++g_mounts.count;
    }
    IrqSpinLockRelease(&g_mountViewLock,flags);
    return 0;
}
static int VfsMountNamespaceBindLocked(MountNamespace* ns,const VfsPath& source,const VfsPath& inputTarget,bool recursive,
    const char* sourceLabel=nullptr,const char* targetLabel=nullptr,const VfsPath* coveredParent=nullptr,const char* coveredName=nullptr)
{
    if(!ns || !source.object || !source.attachment || !inputTarget.object || !inputTarget.attachment)return -22;
    if(!ns->life.Published())return -3;
    VfsMountTargetLease lease;if(int error=lease.Follow(inputTarget))return error;
    const auto& target=lease.value;
    if(VfsVirtual(source))return -95; // synthetic source roots need explicit legacy reporting support
    if(ns->initial && !source.object->ops->directory_path)return -95;
    auto* parent=static_cast<MountEntry*>(target.attachment);
    if(parent->owner!=ns || !VfsMountPublished(*parent))return -22;
    if(parent==ns->root && target.object->identity==parent->rootIdentity && target.object->ops==parent->rootObjectOps)return -95;
    VnodeStat sourceStat{},targetStat{};int error=VfsObjectStatLocked(source.object,&sourceStat);
    if(error)return error;
    if((error=VfsObjectStatLocked(target.object,&targetStat)))return error;
    if(sourceStat.isDir!=targetStat.isDir)return -20;
    if(!sourceStat.isDir) {
        const auto sourceType=sourceStat.mode&0170000,targetType=targetStat.mode&0170000;
        if((sourceType!=0100000 && sourceType!=0140000 && sourceType!=0010000) ||
           (targetType!=0100000 && targetType!=0140000 && targetType!=0010000))return -95;
        // Crossing currently keys on backend identity. Unique targets are
        // unambiguous while link/linkat remain unsupported; attached targets
        // cannot be renamed, unlinked or replaced through VFS mutation gates.
        const bool alreadyUniqueFileMount=parent->coveredParent.object && target.object->identity==parent->rootIdentity && target.object->ops==parent->rootObjectOps;
        if(targetStat.nlink!=1 && !alreadyUniqueFileMount)return -95;
        auto* origin=static_cast<MountEntry*>(source.attachment);
        const bool sourceFileMount=origin->coveredParent.object && source.object->identity==origin->rootIdentity && source.object->ops==origin->rootObjectOps;
        if((sourceStat.nlink!=1 && !sourceFileMount) || !sourceStat.ino)return -95;
        if(!sourceLabel || !targetLabel || !coveredParent || !coveredParent->object || !coveredName || !coveredName[0])return -95;
    }
    if(recursive && sourceStat.isDir)return VfsMountNamespaceRecursiveBindLocked(ns,source,target);
    auto* mount=static_cast<MountEntry*>(kmalloc(sizeof(MountEntry)));if(!mount)return -12;
    *mount={};
    if(sourceStat.isDir) {
        error=VfsGlobalDirectoryLocked(target,mount->mountPoint,sizeof(mount->mountPoint));
        if(!error)error=VfsBackendDirectoryLocked(source,mount->physicalRoot,sizeof(mount->physicalRoot));
    } else {
        if(StrLen(sourceLabel)>=sizeof(mount->physicalRoot) || StrLen(targetLabel)>=sizeof(mount->mountPoint) || StrLen(coveredName)>=sizeof(mount->coveredName))error=-36;
        else {StrCopy(mount->physicalRoot,sourceLabel,sizeof(mount->physicalRoot));StrCopy(mount->mountPoint,targetLabel,sizeof(mount->mountPoint));StrCopy(mount->coveredName,coveredName,sizeof(mount->coveredName));}
    }
    if(error) {kfree(mount);return error;}
    mount->rootDevice=static_cast<MountEntry*>(source.attachment)->rootDevice;mount->attributes=static_cast<MountEntry*>(source.attachment)->attributes;
    mount->owner=ns;mount->referenceState=1;mount->backing=static_cast<MountEntry*>(source.attachment)->backing;
    mount->backing->RetainLocked();mount->rootObject=source.object;VfsObjectRetain(source.object);
    mount->rootIdentity=source.object->identity;mount->rootObjectOps=source.object->ops;mount->rootIdentityKnown=true;
    mount->covered=target;VfsPathRetain(target);
    if(!sourceStat.isDir) {
        mount->coveredParent=*coveredParent;VfsPathRetain(mount->coveredParent);
        auto* origin=static_cast<MountEntry*>(source.attachment);
        mount->rootFileUnlinked=origin->coveredParent.object && origin->rootFileUnlinked;
    }
    mount->linked=true;mount->used=true;mount->publication=++g_mountPublication;
    StrCopy(mount->fsType,static_cast<MountEntry*>(source.attachment)->fsType,sizeof(mount->fsType));
    g_mounts.Publish(mount);return 0;
}
int VfsMountNamespaceBind(MountNamespace* ns,const VfsPath& source,const VfsPath& target,bool recursive)
{
    FileMutationGuard transaction;return VfsMountNamespaceBindLocked(ns,source,target,recursive);
}
// Reporting metadata comes from the same retained walks as the exact operands.
// These names never select/reopen a source or covered target.
static int VfsBindWalkLabelLocked(const VfsOwnedWalk& walk,bool backend,char* out,uint32_t capacity)
{
    const auto& value=walk.value;VnodeStat stat{};
    if(int error=VfsObjectStatLocked(value.object.object,&stat))return error;
    if(stat.isDir)return backend?VfsBackendDirectoryLocked(value.object,out,capacity):VfsGlobalDirectoryLocked(value.object,out,capacity);
    if(backend) {
        auto* mount=static_cast<MountEntry*>(value.object.attachment);
        if(mount->rootObject && value.object.object->identity==mount->rootIdentity && value.object.object->ops==mount->rootObjectOps) {
            if(StrLen(mount->physicalRoot)>=capacity)return -36;
            StrCopy(out,mount->physicalRoot,capacity);return 0;
        }
    }
    if(!value.parent.object)return -2;
    int error=backend?VfsBackendDirectoryLocked(value.parent,out,capacity):VfsGlobalDirectoryLocked(value.parent,out,capacity);
    return error?error:VfsAppendLeaf(out,capacity,value.leaf);
}
// Magic links retain the exact object, but a final link has no lexical parent.
// Recover only its backend entry provenance on the same attachment. A file
// mount root instead belongs to the covered entry, not its source directory.
static int VfsBindEntryLocked(VfsOwnedWalk& walk)
{
    auto& value=walk.value;
    auto* mount=static_cast<MountEntry*>(value.object.attachment);
    if(!mount || !VfsMountPublished(*mount))return -22;
    if(mount->coveredParent.object && value.object.object->identity==mount->rootIdentity &&
       value.object.object->ops==mount->rootObjectOps) {
        VfsPathRelease(value.parent);value.parent=mount->coveredParent;VfsPathRetain(value.parent);
        StrCopy(value.leaf,mount->coveredName,sizeof(value.leaf));return 0;
    }
    if(value.parent.object)return 0;
    auto* object=value.object.object;
    if(!object->ops->entry_parent)return -95;
    VfsObject* parent=nullptr;
    int error=object->ops->entry_parent(object,&parent,value.leaf,sizeof(value.leaf));
    if(error) {VfsObjectRelease(parent);return error;}
    if(!parent || !value.leaf[0]) {VfsObjectRelease(parent);return -5;}
    value.parent={value.object.attachment,parent};VfsPathRetain(value.parent);VfsObjectRelease(parent);
    return 0;
}
int VfsMountNamespaceBindAt(MountNamespace* ns,const VfsPath& root,const VfsPath& start,const char* sourceText,const char* targetText,bool recursive)
{
    FileMutationGuard transaction;VfsOwnedWalk source,target;
    if(int error=VfsWalkAtLocked(root,start,sourceText,source))return error;
    if(int error=VfsWalkAtLocked(root,start,targetText,target))return error;
    VnodeStat stat{};if(int error=VfsObjectStatLocked(source.value.object.object,&stat))return error;
    if(stat.isDir)return VfsMountNamespaceBindLocked(ns,source.value.object,target.value.object,recursive);
    // Mount operations follow covering mounts even through an older FD. Derive
    // covered metadata from that effective target as well, before publication.
    VfsMountTargetLease lease;if(int error=lease.Follow(target.value.object))return error;
    if(!VfsPathSame(lease.value,target.value.object)) {
        VfsPathRelease(target.value.object);target.value.object=lease.value;VfsPathRetain(target.value.object);
        VfsPathRelease(target.value.parent);target.value.parent={};target.value.leaf[0]=0;
    }
    VnodeStat targetStat{};if(int error=VfsObjectStatLocked(target.value.object.object,&targetStat))return error;
    if(targetStat.isDir)return -20;
    if(int error=VfsBindEntryLocked(source))return error;
    if(int error=VfsBindEntryLocked(target))return error;
    char sourceLabel[512],targetLabel[512];
    if(int error=VfsBindWalkLabelLocked(source,true,sourceLabel,sizeof(sourceLabel)))return error;
    if(int error=VfsBindWalkLabelLocked(target,false,targetLabel,sizeof(targetLabel)))return error;
    return VfsMountNamespaceBindLocked(ns,source.value.object,target.value.object,recursive,sourceLabel,targetLabel,&target.value.parent,target.value.leaf);
}
int VfsMountNamespacePivotRoot(MountNamespace* ns,const VfsPath& oldRoot,const VfsPath& newRoot,const VfsPath& putOld)
{
    if(!ns || !oldRoot.object || !newRoot.object || !putOld.object)return -22;
    FileMutationGuard transaction;
    if(!ns->life.Published())return -3;
    if(ns->initial)return -95; // initial legacy/kernel namespace stays fixed
    auto* oldMount=static_cast<MountEntry*>(oldRoot.attachment);
    auto* newMount=static_cast<MountEntry*>(newRoot.attachment);
    auto* putMount=static_cast<MountEntry*>(putOld.attachment);
    if(!oldMount || !newMount || !putMount)return -22;
    if(oldMount!=ns->root || oldMount->owner!=ns || newMount->owner!=ns || putMount->owner!=ns ||
       !VfsMountPublished(*oldMount) || !VfsMountPublished(*newMount) || !VfsMountPublished(*putMount))return -22;
    if(newMount->lockedInherited)return -22;
    if(newMount==oldMount || putMount==oldMount)return -16;
    if(oldRoot.object->identity!=oldMount->rootIdentity || oldRoot.object->ops!=oldMount->rootObjectOps ||
       newRoot.object->identity!=newMount->rootIdentity || newRoot.object->ops!=newMount->rootObjectOps ||
       oldMount->covered.object || !newMount->covered.object)return -22;
    VnodeStat stat{};
    const VfsPath* directoryPaths[2]={&newRoot,&putOld};
    for(const auto* path:directoryPaths) {
        int error=VfsObjectStatLocked(path->object,&stat);if(error)return error;if(!stat.isDir)return -20;
    }
    bool reachable=false;int error=VfsBindContainsLocked(oldRoot,newRoot,&reachable);
    if(error)return error;if(!reachable)return -22;
    error=VfsBindContainsLocked(newRoot,putOld,&reachable);if(error)return error;if(!reachable)return -22;
    char newLabel[512],putLabel[512];
    if((error=VfsGlobalDirectoryLocked(newRoot,newLabel,sizeof(newLabel))))return error;
    if((error=VfsGlobalDirectoryLocked(putOld,putLabel,sizeof(putLabel))))return error;
    if(StrEq(newLabel,"/") || !VfsComponentPrefix(newLabel,putLabel))return -22;
    const char* oldPrefix=putLabel+StrLen(newLabel);if(!*oldPrefix)oldPrefix="/";
    VfsRenameLabels labels;
    for(auto& mount:g_mounts) {
        if(mount.owner!=ns || !VfsMountPublished(mount))continue;
        bool movesWithNew=false;auto* ancestor=&mount;uint32_t depth=0;
        for(;ancestor && depth<VFS_MAX_MOUNT_DEPTH;++depth) {
            if(ancestor==newMount) {movesWithNew=true;break;}
            ancestor=static_cast<MountEntry*>(ancestor->covered.attachment);
        }
        if(depth==VFS_MAX_MOUNT_DEPTH)return -40;
        const char* prefix="";const char* suffix=mount.mountPoint;
        if(movesWithNew) {
            if(!VfsComponentPrefix(newLabel,suffix))return -2;
            suffix+=StrLen(newLabel);if(!*suffix)suffix="/";
        } else {
            prefix=StrEq(oldPrefix,"/")?"":oldPrefix;
            if(StrEq(suffix,"/"))suffix="";
            if(!*prefix && !*suffix)suffix="/";
        }
        if(StrLen(prefix)+StrLen(suffix)>=sizeof(mount.mountPoint))return -36;
        if((error=labels.Add(&mount,prefix,suffix)))return error;
    }
    struct Pivot {
        MountNamespace* ns;MountEntry* oldMount;MountEntry* newMount;
        VfsRenameLabels* labels;VfsPath putOld;VfsPath removedCover;
    } pivot{ns,oldMount,newMount,&labels,putOld,{}};
    VfsPathRetain(putOld);
    auto publish=[](void* argument) {
        auto& p=*static_cast<Pivot*>(argument);
        // Registry -> metadata view. This callback is stores only; release the
        // removed parent path after both short locks have been dropped.
        MountViewGuard view;
        p.removedCover=p.newMount->covered;p.newMount->covered={};
        p.oldMount->covered=p.putOld;p.putOld={};
        p.newMount->lockedInherited=p.oldMount->lockedInherited;p.oldMount->lockedInherited=false;
        p.ns->root=p.newMount;
        for(auto* c=p.labels->head;c;c=c->next)StrCopy(c->mount->mountPoint,c->label,sizeof(c->mount->mountPoint));
    };
    error=FsContextCommitRootMove(oldRoot,newRoot,publish,&pivot);
    VfsPathRelease(pivot.putOld);VfsPathRelease(pivot.removedCover);return error;
}
int VfsMountNamespacePrivatePropagation(MountNamespace* ns,const VfsPath& target)
{
    if(!ns || !target.object || !target.attachment)return -22;
    FileMutationGuard transaction;
    if(!ns->life.Published())return -3;
    auto* mount=static_cast<MountEntry*>(target.attachment);
    if(!VfsMountPublished(*mount) || mount->owner!=ns ||
       target.object->identity!=mount->rootIdentity || target.object->ops!=mount->rootObjectOps)return -22;
    // All construction/copy paths create intrinsically private attachments.
    // Linux PRIVATE->SLAVE without a master remains PRIVATE. Recursing has the
    // same result for every descendant; inherited detach locks stay intact.
    // Do not use this path after introducing shared peers or master links.
    return 0;
}
int VfsMountNamespaceRemount(MountNamespace* ns,const VfsPath& target,uint64_t attributes)
{
    if(!ns || !target.object || !target.attachment)return -22;
    if(attributes!=2 && attributes!=3 && attributes!=6 && attributes!=7)return -22;
    FileMutationGuard transaction;
    if(!ns->life.Published())return -3;
    if(ns->initial)return -95;
    VfsMountTargetLease lease;if(int error=lease.Follow(target))return error;
    auto* mount=static_cast<MountEntry*>(lease.value.attachment);
    if(!VfsMountPublished(*mount) || mount->owner!=ns ||
       lease.value.object->identity!=mount->rootIdentity || lease.value.object->ops!=mount->rootObjectOps)return -22;
    const uint64_t old=VfsMountAttributes(mount);
    if(old&~attributes)return -1;
    // Legacy synthetic storage has no attachment provenance. Only retained
    // roots suppress those brokers; do not claim RO on an unbounded root.
    if((attributes&1) && !(old&1) && !mount->rootObject)return -95;
    // Conservative: even ordinary read handles may trigger atime/writeback.
    // O_PATH and directories carry no data capability. Pending mapping writeback
    // retains its vnode and keeps this count nonzero until final retirement.
    if((attributes&1) && !(old&1) && __atomic_load_n(&mount->nonDirectoryHolds,__ATOMIC_ACQUIRE))return -16;
    MountViewGuard view;__atomic_store_n(&mount->attributes,attributes,__ATOMIC_RELEASE);return 0;
}
int VfsMountNamespaceDetach(MountNamespace* ns,const VfsPath& target)
{
    if(!ns || !target.object || !target.attachment)return -22;
    FileMutationGuard transaction;
    if(!ns->life.Published())return -3;
    // Mount syscalls operate on the top covering mount, even for a retained
    // start path or ".". Ordinary directory-fd traversal keeps its existing
    // view; only this mount target follows down (notably after self-pivot).
    VfsMountTargetLease lease;if(int error=lease.Follow(target))return error;
    auto* selected=static_cast<MountEntry*>(lease.value.attachment);
    if(!VfsMountPublished(*selected) || selected->owner!=ns ||
       lease.value.object->identity!=selected->rootIdentity || lease.value.object->ops!=selected->rootObjectOps)return -22;
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
    MountEntry* removed=nullptr;
    {
        // Readers must see either the whole subtree or none of it. Destruction
        // and covered-path releases stay outside the short metadata lock.
        MountViewGuard view;
        auto** entry=&g_mounts.head;
        while(*entry) {
            auto* mount=*entry;
            if(!mount->detachSelected) {entry=&mount->next;continue;}
            *entry=mount->next;--g_mounts.count;mount->used=false;
            __atomic_fetch_or(&mount->referenceState,MOUNT_DETACHED,__ATOMIC_RELEASE);
            mount->owner=nullptr;mount->linked=false;
            mount->next=removed;removed=mount;
        }
    }
    while(removed) {
        auto* mount=removed;removed=mount->next;
        VfsPath covered=mount->covered;mount->covered={};
    VfsPath coveredParent=mount->coveredParent;mount->coveredParent={};
        mount->next=g_retiredMounts;g_retiredMounts=mount;
        VfsPathRelease(coveredParent);VfsPathRelease(covered);VfsMountDrop(mount);
    }
    VfsCollectRetiredMountsLocked();return 0;
}

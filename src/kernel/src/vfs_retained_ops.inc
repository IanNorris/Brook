// Included after mapping-registry definitions, within namespace brook.
struct VfsOwnedWalk {
    RetainedWalkResult<VfsPath> value{};
    ~VfsOwnedWalk() { VfsPathRelease(value.object);VfsPathRelease(value.parent); }
};
struct VfsOwnedObject {
    VfsObject* value=nullptr;
    ~VfsOwnedObject() { VfsObjectRelease(value); }
};
static int VfsWalkAtLocked(const VfsPath& root,const VfsPath& start,const char* text,
                            VfsOwnedWalk& walk,PathWalkOptions options={},bool realIds=false)
{
    if(!root.object || !start.object)return -9;
    VfsRetainedProvider provider{realIds};return WalkRetainedPath(root,start,text,&walk.value,provider,options);
}
static int VfsMutationLeaf(const VfsPath& parent,const char* name,bool requireWritable=true)
{
    if(!parent.object)return -16;
    if(requireWritable && VfsReadOnly(static_cast<MountEntry*>(parent.attachment)))return -30;
    if(!name || !name[0] || StrEq(name,".") || StrEq(name,".."))return -22;
    for(uint32_t i=0;name[i];++i)if(name[i]=='/')return -22;
    return 0;
}
static bool VfsVirtualChildrenLocked(const VfsPath& path)
{
    for(const auto& mount:g_mounts) {
        if(!VfsMountPublished(mount))continue;
        VfsPath current=mount.covered;
        while(VfsVirtual(current)) {
            auto* directory=static_cast<VfsVirtualDirectory*>(current.object->priv);
            if(VfsPathSame(directory->parent,path))return true;
            current=directory->parent;
        }
    }
    return false;
}
// Return an independently owned uncovered child, or empty for a missing leaf.
// Every error releases its partial backend output. Parent pins backing through
// the use and final release of this temporary backend object.
static int VfsMutationTargetLocked(const VfsPath& parent,const char* name,VfsOwnedObject& child,bool requireWritable=true)
{
    if(int error=VfsMutationLeaf(parent,name,requireWritable))return error;
    VfsPath reserved{};bool virtualLink=VfsReservedVirtualLocked(parent,name,&reserved);
    VfsPathRelease(reserved);if(virtualLink)return -16;
    if(!parent.object->ops->lookup)return -95;
    int error=parent.object->ops->lookup(parent.object,name,&child.value,false);
    if(error) { VfsObjectRelease(child.value);child.value=nullptr;return error==-2?0:error; }
    if(!child.value)return -5;
    for(const auto& mount:g_mounts)
        if(VfsMountPublished(mount) && VfsPathSame(mount.covered,{parent.attachment,child.value}))return -16;
    return 0;
}
static int VfsObjectStatLocked(VfsObject* object,VnodeStat* stat)
{
    if(!stat)return -22;
    *stat={};return object && object->ops->stat?object->ops->stat(object,stat):-95;
}
Vnode* VfsOpenAt(const VfsPath& root,const VfsPath& start,const char* text,int flags,
                 uint16_t mode,uint16_t mask,int* error,bool followFinal)
{
    int local=0;if(!error)error=&local;*error=0;
    constexpr int allowed=VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC|VFS_O_APPEND|VFS_O_EXCL|VFS_O_READWRITE|VFS_O_METADATA|VFS_O_EXEC;
    if((flags&~allowed) || ((flags&VFS_O_METADATA) && (flags&~VFS_O_METADATA)) ||
       ((flags&VFS_O_READWRITE) && !(flags&VFS_O_WRITE)) ||
       ((flags&VFS_O_EXEC) && (flags&~VFS_O_EXEC))) { *error=-22;return nullptr; }
    FileMutationGuard mutation(flags&(VFS_O_CREATE|VFS_O_TRUNC));VfsOwnedWalk walk;
    bool exclusive=(flags&(VFS_O_CREATE|VFS_O_EXCL))==(VFS_O_CREATE|VFS_O_EXCL);
    *error=VfsWalkAtLocked(root,start,text,walk,{followFinal&&!exclusive,bool(flags&VFS_O_CREATE),false,-21});
    if(*error)return nullptr;
    auto& result=walk.value;
    if(result.object.object) {
        if(exclusive) { *error=-17;return nullptr; }
        if(flags&VFS_O_CREATE) {
            VnodeStat stat{};*error=VfsObjectStatLocked(result.object.object,&stat);
            if(*error)return nullptr;
            if(stat.isDir) { *error=-21;return nullptr; }
        }
        return VfsPathOpenLocked(result.object,flags&~(VFS_O_CREATE|VFS_O_EXCL),error);
    }
    if(!result.missing) { *error=-2;return nullptr; }
    VfsOwnedObject target;*error=VfsMutationTargetLocked(result.parent,result.leaf,target);
    if(*error)return nullptr;
    if(target.value) { *error=-17;return nullptr; }
    auto* ops=result.parent.object->ops;
    if(!ops->create) { *error=-95;return nullptr; }
    int createFlags=flags&~(VFS_O_CREATE|VFS_O_EXCL|VFS_O_TRUNC);
    if(flags&VFS_O_TRUNC) { if(!(createFlags&VFS_O_WRITE))createFlags|=VFS_O_READWRITE;createFlags|=VFS_O_WRITE; }
    Vnode* node=ops->create(result.parent.object,result.leaf,createFlags,mode&07777&~(mask&0777),error);
    if(!node) { if(!*error)*error=-5;return nullptr; }
    auto* mount=static_cast<MountEntry*>(result.parent.attachment);
    VfsAttachMount(node,mount,createFlags);
    // A newly created ext2 vnode does not yet cache its backend identity.
    // Capture it before publishing the descriptor, as ordinary retained opens
    // already do. Proc-fd following must acquire references under the FD lock
    // without invoking backend callbacks or reopening a reported pathname.
    if(!node->backendObject && mount->backing->fsOps->vnode_object) {
        VfsObject* object=nullptr;
        *error=mount->backing->fsOps->vnode_object(node,&object);
        if(*error || !object) {
            VfsObjectRelease(object);VfsClose(node);
            if(!*error)*error=-5;
            return nullptr;
        }
        node->backendObject=object;
    }
    return node;
}
int VfsOpenExecutableAt(const VfsPath& root,const VfsPath& start,const char* text,
                        Vnode** out,char* resolved,uint32_t capacity)
{
    if(!out || !resolved || capacity<2)return -22;
    *out=nullptr;resolved[0]=0;
    FileMutationGuard lookup(false);VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk))return error;
    VnodeStat stat{};if(int error=VfsObjectStatLocked(walk.value.object.object,&stat))return error;
    if(stat.isDir)return -13;
    // Reporting happens from the completed retained walk; it never reopens
    // the executable by a global name, including beneath an overmount.
    if(!walk.value.parent.object)return -2;
    if(int error=VfsGlobalDirectoryLocked(walk.value.parent,resolved,capacity))return error;
    uint32_t prefix=StrLen(resolved),leaf=StrLen(walk.value.leaf);
    if(prefix==1 && resolved[0]=='/')prefix=0;
    if(prefix+leaf+1>=capacity) {resolved[0]=0;return -36;}
    resolved[prefix++]='/';StrCopy(resolved+prefix,walk.value.leaf,capacity-prefix);
    int error=0;Vnode* node=VfsPathOpenLocked(walk.value.object,VFS_O_EXEC,&error);
    if(!node)return error?error:-5;
    *out=node;return 0;
}
int VfsStatAt(const VfsPath& root,const VfsPath& start,const char* text,VnodeStat* out,bool followFinal)
{
    if(!out)return -22;
    *out={};FileMutationGuard lookup(false);VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{followFinal}))return error;
    return VfsObjectStatLocked(walk.value.object.object,out);
}
int VfsAccessAt(const VfsPath& root,const VfsPath& start,const char* text,uint8_t required,bool realIds,bool followFinal)
{
    if(required&~7u)return -22;
    FileMutationGuard lookup(false);VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{followFinal},realIds))return error;
    auto* object=walk.value.object.object;
    if((required&2) && VfsReadOnly(static_cast<MountEntry*>(walk.value.object.attachment))) {
        VnodeStat stat{};if(int error=VfsObjectStatLocked(object,&stat))return error;
        if((stat.mode&0170000)!=0140000 && (stat.mode&0170000)!=0010000)return -30;
    }
    return object->ops->access?object->ops->access(object,required,realIds):-95;
}
int VfsReadlinkAt(const VfsPath& root,const VfsPath& start,const char* text,char* out,uint32_t capacity)
{
    if(!out || !capacity)return -22;
    FileMutationGuard lookup(false);VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{false}))return error;
    auto* object=walk.value.object.object;return object->ops->readlink?object->ops->readlink(object,out,capacity):-95;
}
int VfsPathReadlink(const VfsPath& path,char* out,uint32_t capacity)
{
    if(!out || !capacity)return -22;
    if(!path.object || !path.attachment)return -9;
    FileMutationGuard lookup(false);
    VnodeStat stat{};if(int error=VfsObjectStatLocked(path.object,&stat))return error;
    // Linux readlinkat(fd, "") returns ENOENT for a non-symlink FD.
    if(!stat.isSymlink)return -2;
    return path.object->ops->readlink?path.object->ops->readlink(path.object,out,capacity):-95;
}
int VfsMkdirAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode,uint16_t mask)
{
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{false,true,true}))return error;
    auto& parent=walk.value.parent;const char* leaf=walk.value.leaf;
    if(!parent.object || StrEq(leaf,".") || StrEq(leaf,".."))return -17;
    // mkdir(existing) is EEXIST even on a read-only attachment. Container
    // mkdir-p preparation relies on this without requesting any mutation.
    VfsOwnedObject target;if(int error=VfsMutationTargetLocked(parent,leaf,target,false))return error==-16?-17:error;
    if(target.value)return -17;
    if(VfsReadOnly(static_cast<MountEntry*>(parent.attachment)))return -30;
    return parent.object->ops->mkdir?parent.object->ops->mkdir(parent.object,leaf,mode&01777&~(mask&0777)):-95;
}
int VfsRemoveAt(const VfsPath& root,const VfsPath& start,const char* text,bool directory)
{
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{false,false,true}))return error;
    auto& parent=walk.value.parent;const char* leaf=walk.value.leaf;
    VfsOwnedObject target;if(int error=VfsMutationTargetLocked(parent,leaf,target))return error;
    if(!target.value)return -2;
    VnodeStat stat{};if(int error=VfsObjectStatLocked(target.value,&stat))return error;
    if(walk.value.trailing && (!stat.isDir || stat.isSymlink))return -20;
    if(stat.isDir!=directory)return directory?-20:-21;
    if(directory && VfsVirtualChildrenLocked({parent.attachment,target.value}))return -39;
    if(VfsMappedMutationBusy(stat))return -16;
    int error=parent.object->ops->remove?parent.object->ops->remove(parent.object,leaf,directory):-95;
    if(!error && !stat.isDir)VfsMarkFileRootsUnlinkedLocked(static_cast<MountEntry*>(parent.attachment)->backing,stat.ino);
    return error;
}
int VfsSymlinkAt(const VfsPath& root,const VfsPath& start,const char* targetText,const char* text)
{
    if(!targetText || !targetText[0])return -22;
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{false,true,true}))return error;
    auto& parent=walk.value.parent;const char* leaf=walk.value.leaf;
    VfsOwnedObject target;if(int error=VfsMutationTargetLocked(parent,leaf,target))return error;
    if(target.value)return -17;
    if(walk.value.trailing)return -2;
    return parent.object->ops->symlink?parent.object->ops->symlink(parent.object,targetText,leaf):-95;
}
Vnode* VfsCreateFifoAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode,uint16_t mask,int* error)
{
    int local=0;if(!error)error=&local;*error=0;
    FileMutationGuard mutation;VfsOwnedWalk walk;
    *error=VfsWalkAtLocked(root,start,text,walk,{false,true,true});if(*error)return nullptr;
    auto& parent=walk.value.parent;const char* leaf=walk.value.leaf;
    VfsOwnedObject target;*error=VfsMutationTargetLocked(parent,leaf,target);if(*error)return nullptr;
    if(target.value) { *error=-17;return nullptr; }
    if(walk.value.trailing) { *error=-2;return nullptr; }
    if(!parent.object->ops->create_fifo) { *error=-95;return nullptr; }
    Vnode* node=parent.object->ops->create_fifo(parent.object,leaf,mode&07777&~(mask&0777),error);
    if(!node) { if(!*error)*error=-5;return nullptr; }
    VfsAttachMount(node,static_cast<MountEntry*>(parent.attachment),0);return node;
}
Vnode* VfsCreateSocketAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode,uint16_t mask,int* error)
{
    int local=0;if(!error)error=&local;*error=0;
    FileMutationGuard mutation;VfsOwnedWalk walk;
    *error=VfsWalkAtLocked(root,start,text,walk,{false,true,true});if(*error)return nullptr;
    auto& parent=walk.value.parent;const char* leaf=walk.value.leaf;
    VfsOwnedObject target;*error=VfsMutationTargetLocked(parent,leaf,target);if(*error)return nullptr;
    if(target.value) { *error=-98;return nullptr; }
    if(walk.value.trailing) { *error=-2;return nullptr; }
    if(!parent.object->ops->create_socket) { *error=-95;return nullptr; }
    Vnode* node=parent.object->ops->create_socket(parent.object,leaf,mode&0777&~(mask&0777),error);
    if(!node) { if(!*error)*error=-5;return nullptr; }
    VfsAttachMount(node,static_cast<MountEntry*>(parent.attachment),0);return node;
}
static bool VfsComponentPrefix(const char* prefix,const char* text)
{
    while(*prefix && *text==*prefix) { ++prefix;++text; }
    return !*prefix && (!*text || *text=='/');
}
static int VfsAppendLeaf(char* path,uint32_t capacity,const char* leaf)
{
    uint32_t length=StrLen(path),suffix=StrLen(leaf);
    if(length==1 && path[0]=='/')length=0;
    if(length+suffix+1>=capacity)return -36;
    path[length++]='/';StrCopy(path+length,leaf,capacity-length);return 0;
}
// Predict reporting names from retained edges, rather than translating the
// caller's namespace label. A backing rename is visible through every copied
// attachment, including bind roots with a different physical prefix.
static int VfsRenamePhysicalLocked(MountBacking* current,MountBacking* renamed,
                                  const char* source,const char* target,char* path,uint32_t capacity)
{
    if(current!=renamed || !VfsComponentPrefix(source,path))return 0;
    char suffix[512];StrCopy(suffix,path+StrLen(source),sizeof(suffix));
    if(StrLen(target)+StrLen(suffix)>=capacity)return -36;
    StrCopy(path,target,capacity);StrCopy(path+StrLen(target),suffix,capacity-StrLen(target));return 0;
}
static bool VfsRenameMayMoveMountLabelLocked(const MountEntry* mount,MountBacking* backing)
{
    auto* ancestor=static_cast<MountEntry*>(mount->covered.attachment);
    for(uint32_t depth=0;ancestor && depth<VFS_MAX_MOUNT_DEPTH;++depth) {
        if(ancestor->backing==backing)return true;
        ancestor=static_cast<MountEntry*>(ancestor->covered.attachment);
    }
    return false;
}
static int VfsProjectedRenameDirectoryLocked(VfsPath path,MountBacking* backing,
                                              const char* source,const char* target,char* out,uint32_t capacity)
{
    // Iterate towards the namespace root. No recursion proportional to mount
    // depth, and no allocation or backend work under the metadata view lock.
    char suffix[512]{};
    for(uint32_t depth=0;depth<VFS_MAX_MOUNT_DEPTH;++depth) {
        auto* attachment=static_cast<MountEntry*>(path.attachment);
        char physical[512];int error=VfsBackendDirectoryLocked(path,physical,sizeof(physical));
        if(error)return error;
        if((error=VfsRenamePhysicalLocked(attachment->backing,backing,source,target,physical,sizeof(physical))))return error;
        const char* relative=physical;
        if(attachment->rootObject) {
            char root[512];auto* object=attachment->rootObject;
            if(!object->ops->directory_path)return -95;
            if((error=object->ops->directory_path(object,root,sizeof(root))))return error;
            if((error=VfsRenamePhysicalLocked(attachment->backing,backing,source,target,root,sizeof(root))))return error;
            if(StrLen(root)>1) {
                if(!VfsComponentPrefix(root,physical))return -2;
                relative=physical+StrLen(root);
            }
        }
        uint32_t length=StrLen(relative);if(length==1 && relative[0]=='/')length=0;
        const uint32_t tail=StrLen(suffix);if(length+tail>=sizeof(suffix))return -36;
        for(uint32_t i=tail+1;i;--i)suffix[length+i-1]=suffix[i-1];
        for(uint32_t i=0;i<length;++i)suffix[i]=relative[i];
        // Anchor at an unaffected attachment label. Reconstructing all the
        // way through legacy case-insensitive ancestors can change /tmp to
        // /TMP during an unrelated rename inside a mounted tmpfs.
        if(attachment->linked && attachment->covered.object &&
           VfsRenameMayMoveMountLabelLocked(attachment,backing)) {path=attachment->covered;continue;}
        uint32_t prefix=StrLen(attachment->mountPoint);
        if(prefix==1 && attachment->mountPoint[0]=='/')prefix=0;
        uint32_t total=prefix+StrLen(suffix);
        if((total?total:1)>=capacity)return -36;
        for(uint32_t i=0;i<prefix;++i)out[i]=attachment->mountPoint[i];
        StrCopy(out+prefix,suffix,capacity-prefix);
        if(!total) {out[0]='/';out[1]=0;}
        return 0;
    }
    return -40;
}
static int VfsPrepareBackingRenameLabelsLocked(MountBacking* backing,const char* oldPhysical,
                                               const char* newPhysical,VfsRenameLabels& labels,uint64_t exactFileInode)
{
    char source[512],target[512];
    if(StrLen(oldPhysical)+1>=sizeof(source) || StrLen(newPhysical)+1>=sizeof(target))return -36;
    source[0]=target[0]='/';
    StrCopy(source+(oldPhysical[0]=='/'?0:1),oldPhysical,sizeof(source)-1);
    StrCopy(target+(newPhysical[0]=='/'?0:1),newPhysical,sizeof(target)-1);
    for(auto& mount:g_mounts) {
        if(!VfsMountPublished(mount))continue;
        // A source rename changes a bind's physical root even when its target
        // label is unchanged, including when that bind is the namespace root.
        char physical[512];StrCopy(physical,mount.physicalRoot,sizeof(physical));
        int error=0;
        if(!mount.rootFileUnlinked && (!exactFileInode || (mount.coveredParent.object && mount.rootIdentity==exactFileInode)))
            error=VfsRenamePhysicalLocked(mount.backing,backing,source,target,physical,sizeof(physical));
        if(error)return error;
        char projected[512];StrCopy(projected,mount.mountPoint,sizeof(projected));
        bool relevant=!exactFileInode && VfsRenameMayMoveMountLabelLocked(&mount,backing);
        if(relevant && mount.linked && mount.covered.object) {
            error=VfsProjectedRenameDirectoryLocked(mount.coveredParent.object?mount.coveredParent:mount.covered,backing,source,target,projected,sizeof(projected));
            if(!error && mount.coveredParent.object)error=VfsAppendLeaf(projected,sizeof(projected),mount.coveredName);
            if(error)return error;
        }
        if(StrEq(projected,mount.mountPoint) && StrEq(physical,mount.physicalRoot))continue;
        if(StrLen(projected)>=sizeof(mount.mountPoint))return -36;
        if((error=labels.Add(&mount,projected,"",physical)))return error;
    }
    return 0;
}
static int VfsPrepareRenameLabelsLocked(const VfsPath& oldParent,const char* oldName,
                                        const VfsPath& newParent,const char* newName,VfsRenameLabels& labels,uint64_t exactFileInode=0)
{
    if(exactFileInode) {
        bool related=false;auto* backing=static_cast<MountEntry*>(oldParent.attachment)->backing;
        for(const auto& mount:g_mounts)if(VfsMountPublished(mount) && mount.coveredParent.object && !mount.rootFileUnlinked && mount.backing==backing && mount.rootIdentity==exactFileInode) {related=true;break;}
        if(!related)return 0;
    }
    char source[512],target[512];int error=VfsBackendDirectoryLocked(oldParent,source,sizeof(source));
    if(error)return error;
    if((error=VfsAppendLeaf(source,sizeof(source),oldName)))return error;
    if((error=VfsBackendDirectoryLocked(newParent,target,sizeof(target))))return error;
    if((error=VfsAppendLeaf(target,sizeof(target),newName)))return error;
    return VfsPrepareBackingRenameLabelsLocked(static_cast<MountEntry*>(oldParent.attachment)->backing,source,target,labels,exactFileInode);
}
int VfsRenameAt(const VfsPath& root,const VfsPath& oldStart,const char* oldText,const VfsPath& newStart,const char* newText)
{
    FileMutationGuard mutation;VfsOwnedWalk oldWalk,newWalk;
    if(int error=VfsWalkAtLocked(root,oldStart,oldText,oldWalk,{false,false,true}))return error;
    if(int error=VfsWalkAtLocked(root,newStart,newText,newWalk,{false,true,true}))return error;
    auto& oldParent=oldWalk.value.parent;auto& newParent=newWalk.value.parent;
    if(!oldParent.object || !newParent.object)return -16;
    if(oldParent.attachment!=newParent.attachment)return -18;
    const char* oldName=oldWalk.value.leaf;const char* newName=newWalk.value.leaf;
    VfsOwnedObject source,target;
    if(int error=VfsMutationTargetLocked(oldParent,oldName,source))return error;
    if(!source.value)return -2;
    if(int error=VfsMutationTargetLocked(newParent,newName,target))return error;
    VnodeStat oldStat{},newStat{};
    if(int error=VfsObjectStatLocked(source.value,&oldStat))return error;
    if(target.value)if(int error=VfsObjectStatLocked(target.value,&newStat))return error;
    if((oldWalk.value.trailing || newWalk.value.trailing) && (!oldStat.isDir || oldStat.isSymlink))return -20;
    if(newWalk.value.trailing && target.value && (!newStat.isDir || newStat.isSymlink))return -20;
    const bool same=target.value && VfsPathSame({oldParent.attachment,source.value},{newParent.attachment,target.value});
    if(!same && target.value && newStat.isDir && VfsVirtualChildrenLocked({newParent.attachment,target.value}))return -39;
    if(VfsMappedMutationBusy(oldStat) || (target.value && VfsMappedMutationBusy(newStat)))return -16;
    auto* ops=oldParent.object->ops;if(!ops->rename || ops!=newParent.object->ops)return -95;
    VfsRenameLabels labels;
    if(!same && (oldStat.isDir || (oldStat.mode&0170000)==0100000 || (oldStat.mode&0170000)==0140000 || (oldStat.mode&0170000)==0010000))if(int error=VfsPrepareRenameLabelsLocked(oldParent,oldName,newParent,newName,labels,oldStat.isDir?0:oldStat.ino))return error;
    int error=ops->rename(oldParent.object,oldName,newParent.object,newName);
    if(!error) {
        labels.Apply();
        if(!same && target.value && !newStat.isDir)
            VfsMarkFileRootsUnlinkedLocked(static_cast<MountEntry*>(newParent.attachment)->backing,newStat.ino);
    }
    return error;
}
int VfsChmodAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode)
{
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk))return error;
    if(VfsReadOnly(static_cast<MountEntry*>(walk.value.object.attachment)))return -30;
    auto* object=walk.value.object.object;return object->ops->chmod?object->ops->chmod(object,mode):-95;
}
int VfsChownAt(const VfsPath& root,const VfsPath& start,const char* text,uint32_t uid,uint32_t gid,bool followFinal)
{
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{followFinal}))return error;
    if(VfsReadOnly(static_cast<MountEntry*>(walk.value.object.attachment)))return -30;
    auto* object=walk.value.object.object;return object->ops->chown?object->ops->chown(object,uid,gid):-95;
}
int VfsSetTimesAt(const VfsPath& root,const VfsPath& start,const char* text,const VnodeTime* times,bool followFinal)
{
    if(!times)return -22;
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{followFinal}))return error;
    if(VfsReadOnly(static_cast<MountEntry*>(walk.value.object.attachment)))return -30;
    auto* object=walk.value.object.object;return object->ops->set_times?object->ops->set_times(object,times):-95;
}

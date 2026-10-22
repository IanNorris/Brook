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
static int VfsMutationLeaf(const VfsPath& parent,const char* name)
{
    if(!parent.object)return -16;
    if(!name || !name[0] || StrEq(name,".") || StrEq(name,".."))return -22;
    for(uint32_t i=0;name[i];++i)if(name[i]=='/')return -22;
    return 0;
}
static bool VfsVirtualChildrenLocked(const VfsPath& path)
{
    for(const auto& mount:g_mounts) {
        if(!mount.used)continue;
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
static int VfsMutationTargetLocked(const VfsPath& parent,const char* name,VfsOwnedObject& child)
{
    if(int error=VfsMutationLeaf(parent,name))return error;
    VfsPath reserved{};bool virtualLink=VfsReservedVirtualLocked(parent,name,&reserved);
    VfsPathRelease(reserved);if(virtualLink)return -16;
    if(!parent.object->ops->lookup)return -95;
    int error=parent.object->ops->lookup(parent.object,name,&child.value,false);
    if(error) { VfsObjectRelease(child.value);child.value=nullptr;return error==-2?0:error; }
    if(!child.value)return -5;
    for(const auto& mount:g_mounts)
        if(mount.used && VfsPathSame(mount.covered,{parent.attachment,child.value}))return -16;
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
    VfsAttachMount(node,static_cast<MountEntry*>(result.parent.attachment));return node;
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
    auto* object=walk.value.object.object;return object->ops->access?object->ops->access(object,required,realIds):-95;
}
int VfsReadlinkAt(const VfsPath& root,const VfsPath& start,const char* text,char* out,uint32_t capacity)
{
    if(!out || !capacity)return -22;
    FileMutationGuard lookup(false);VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{false}))return error;
    auto* object=walk.value.object.object;return object->ops->readlink?object->ops->readlink(object,out,capacity):-95;
}
int VfsMkdirAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode,uint16_t mask)
{
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{false,true,true}))return error;
    auto& parent=walk.value.parent;const char* leaf=walk.value.leaf;
    if(!parent.object || StrEq(leaf,".") || StrEq(leaf,".."))return -17;
    VfsOwnedObject target;if(int error=VfsMutationTargetLocked(parent,leaf,target))return error==-16?-17:error;
    if(target.value)return -17;
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
    return parent.object->ops->remove?parent.object->ops->remove(parent.object,leaf,directory):-95;
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
    VfsAttachMount(node,static_cast<MountEntry*>(parent.attachment));return node;
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
struct VfsRenameLabels { bool changed[VFS_MAX_MOUNTS]{};char labels[VFS_MAX_MOUNTS][64]{}; };
static int VfsPrepareRenameLabelsLocked(const VfsPath& oldParent,const char* oldName,
                                        const VfsPath& newParent,const char* newName,VfsRenameLabels& labels)
{
    char sourceRelative[512],sourceGlobal[512],targetGlobal[512];
    int error=VfsBackendDirectoryLocked(oldParent,sourceRelative,sizeof(sourceRelative));if(error)return error;
    if((error=VfsAppendLeaf(sourceRelative,sizeof(sourceRelative),oldName)))return error;
    if((error=VfsGlobalDirectoryLocked(oldParent,sourceGlobal,sizeof(sourceGlobal))))return error;
    if((error=VfsAppendLeaf(sourceGlobal,sizeof(sourceGlobal),oldName)))return error;
    if((error=VfsGlobalDirectoryLocked(newParent,targetGlobal,sizeof(targetGlobal))))return error;
    if((error=VfsAppendLeaf(targetGlobal,sizeof(targetGlobal),newName)))return error;
    const uint32_t oldLength=StrLen(sourceGlobal),newLength=StrLen(targetGlobal);
    for(uint32_t i=0;i<VFS_MAX_MOUNTS;++i) {
        const auto& mount=g_mounts[i];if(!mount.used)continue;
        VfsPath covered=mount.covered;
        for(uint32_t depth=0;covered.object && covered.attachment!=oldParent.attachment && depth<VFS_MAX_MOUNTS;++depth)
            covered=static_cast<MountEntry*>(covered.attachment)->covered;
        if(!covered.object || covered.attachment!=oldParent.attachment)continue;
        char physical[512];if((error=VfsBackendDirectoryLocked(covered,physical,sizeof(physical))))return error;
        if(!VfsComponentPrefix(sourceRelative,physical))continue;
        if(!VfsComponentPrefix(sourceGlobal,mount.mountPoint))return -5;
        if(newLength+StrLen(mount.mountPoint+oldLength)>=sizeof(labels.labels[i]))return -36;
        labels.changed[i]=true;StrCopy(labels.labels[i],targetGlobal,sizeof(labels.labels[i]));
        StrCopy(labels.labels[i]+newLength,mount.mountPoint+oldLength,sizeof(labels.labels[i])-newLength);
    }
    return 0;
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
    if(!same && oldStat.isDir)if(int error=VfsPrepareRenameLabelsLocked(oldParent,oldName,newParent,newName,labels))return error;
    int error=ops->rename(oldParent.object,oldName,newParent.object,newName);
    if(!error)for(uint32_t i=0;i<VFS_MAX_MOUNTS;++i)
        if(labels.changed[i])StrCopy(g_mounts[i].mountPoint,labels.labels[i],sizeof(g_mounts[i].mountPoint));
    return error;
}
int VfsChmodAt(const VfsPath& root,const VfsPath& start,const char* text,uint16_t mode)
{
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk))return error;
    auto* object=walk.value.object.object;return object->ops->chmod?object->ops->chmod(object,mode):-95;
}
int VfsChownAt(const VfsPath& root,const VfsPath& start,const char* text,uint32_t uid,uint32_t gid,bool followFinal)
{
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{followFinal}))return error;
    auto* object=walk.value.object.object;return object->ops->chown?object->ops->chown(object,uid,gid):-95;
}
int VfsSetTimesAt(const VfsPath& root,const VfsPath& start,const char* text,const VnodeTime* times,bool followFinal)
{
    if(!times)return -22;
    FileMutationGuard mutation;VfsOwnedWalk walk;
    if(int error=VfsWalkAtLocked(root,start,text,walk,{followFinal}))return error;
    auto* object=walk.value.object.object;return object->ops->set_times?object->ops->set_times(object,times):-95;
}

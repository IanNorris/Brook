// Included within namespace brook after MountEntry and string helpers.
// All ...Locked operations require the outer VFS namespace transaction.
// A virtual directory reserves an otherwise missing mount ancestor. Its name
// link exists only while a published attachment references its chain; a held
// detached object can outlive that link without becoming lookup authority.
struct VfsVirtualDirectory { VfsObject object; VfsPath parent; char name[256]; };
extern const VfsObjectOps g_virtualDirectoryOps;
// Provisioned once before userspace; retains the exact canonical host /dev.
static VfsPath g_nullDeviceAnchor;
static bool VfsVirtual(const VfsPath& p) { return p.object && p.object->ops==&g_virtualDirectoryOps; }

bool VfsPathSame(const VfsPath& a,const VfsPath& b)
{
    return a.attachment==b.attachment && a.object && b.object &&
           a.object->identity==b.object->identity && a.object->ops==b.object->ops;
}
static void VfsPathHoldMount(MountEntry* mount)
{
    VfsMountHold(mount);
    uint64_t count=__atomic_load_n(&mount->pathHolds,__ATOMIC_RELAXED);
    do { if(count==~uint64_t{0})__builtin_trap(); }
    while(!__atomic_compare_exchange_n(&mount->pathHolds,&count,count+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
}
void VfsPathRetain(const VfsPath& path)
{
    if(!path.object) { if(path.attachment)__builtin_trap();return; }
    if(!path.attachment)__builtin_trap();
    VfsPathHoldMount(static_cast<MountEntry*>(path.attachment));VfsObjectRetain(path.object);
}
void VfsPathRelease(VfsPath path)
{
    if(!path.object) { if(path.attachment)__builtin_trap();return; }
    auto* mount=static_cast<MountEntry*>(path.attachment);
    if(!mount)__builtin_trap();
    VfsObjectRelease(path.object); // Backend backing remains pinned through destroy.
    if(!__atomic_fetch_sub(&mount->pathHolds,uint64_t{1},__ATOMIC_ACQ_REL))__builtin_trap();
    VfsMountDrop(mount);
}
static int VfsMountRootLocked(MountEntry* mount,VfsPath* out)
{
    *out={};
    if(!mount || !mount->backing)return -2;
    if(mount->rootObject) { VfsPathHoldMount(mount);VfsObjectRetain(mount->rootObject);*out={mount,mount->rootObject};return 0; }
    if(!mount->rootIdentityKnown || !mount->backing->fsOps->root_object)return -95;
    VfsPathHoldMount(mount);
    VfsObject* object=nullptr;
    int error=mount->backing->fsOps->root_object(mount->backing->mountPriv,mount->backing->pdrv,&object);
    if(!error && (!object || object->identity!=mount->rootIdentity || object->ops!=mount->rootObjectOps))error=-5;
    if(error) {
        VfsObjectRelease(object);
        __atomic_fetch_sub(&mount->pathHolds,uint64_t{1},__ATOMIC_ACQ_REL);VfsMountDrop(mount);
        return error;
    }
    *out={mount,object};return 0;
}
int VfsPathRoot(VfsPath* out)
{
    if(!out)return -22;
    *out={};FileMutationGuard lookup(false);
    for(const auto& mount:g_mounts)if(VfsMountPublished(mount) && mount.owner==&g_initialNamespace && !mount.linked)return -95;
    return VfsMountRootLocked(g_rootMount,out);
}
static void VfsVirtualDestroy(VfsObject* object)
{
    auto* directory=static_cast<VfsVirtualDirectory*>(object->priv);
    VfsPath parent=directory->parent;kfree(directory);VfsPathRelease(parent);
}
static int VfsVirtualStat(VfsObject* object,VnodeStat* out)
{
    if(!out)return -22;
    *out={};out->isDir=true;out->mode=0040555;out->ino=object->identity;return 0;
}
static int VfsVirtualAccess(VfsObject*,uint8_t required,bool) { return required&2?-30:0; }
static int VfsVirtualChild(VfsObject*,const char*,VfsObject** out,bool) { *out=nullptr;return -2; }
static int VfsVirtualParent(VfsObject* object,VfsObject** out,bool)
{
    auto* directory=static_cast<VfsVirtualDirectory*>(object->priv);
    *out=directory->parent.object;VfsObjectRetain(*out);return 0;
}
static int VfsVirtualReadlink(VfsObject*,char*,uint32_t) { return -22; }
static int VfsVirtualNodeStat(Vnode* node,VnodeStat* out) { return VfsVirtualStat(node->backendObject,out); }
static const VnodeOps g_virtualNodeOps={
    .open=nullptr,.read=nullptr,.write=nullptr,.readdir=nullptr,.close=nullptr,
    .stat=VfsVirtualNodeStat,.fsync=nullptr,
};
static Vnode* VfsVirtualOpen(VfsObject* object,int flags,int* error)
{
    *error=0;
    if(flags & ~(VFS_O_WRITE|VFS_O_READWRITE|VFS_O_METADATA|VFS_O_EXEC|VFS_O_APPEND)) { *error=-22;return nullptr; }
    if(flags & VFS_O_WRITE) { *error=-21;return nullptr; }
    if(flags & VFS_O_EXEC) { *error=-13;return nullptr; }
    auto* node=static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
    if(!node) { *error=-12;return nullptr; }
    *node={};node->ops=&g_virtualNodeOps;node->type=VnodeType::Dir;node->refCount=1;
    VfsObjectRetain(object);node->backendObject=object;return node;
}
const VfsObjectOps g_virtualDirectoryOps={VfsVirtualDestroy,VfsVirtualStat,VfsVirtualAccess,
    VfsVirtualChild,VfsVirtualParent,VfsVirtualReadlink,VfsVirtualOpen};
static int VfsMakeVirtualLocked(const VfsPath& parent,const char* name,VfsPath* out)
{
    *out={};auto* directory=static_cast<VfsVirtualDirectory*>(kmalloc(sizeof(VfsVirtualDirectory)));
    if(!directory)return -12;
    *directory={};directory->object={&g_virtualDirectoryOps,directory,reinterpret_cast<uint64_t>(directory),1};
    directory->parent=parent;VfsPathRetain(parent);StrCopy(directory->name,name,sizeof(directory->name));
    // Adopt the newly created object while separately retaining its attachment.
    auto* mount=static_cast<MountEntry*>(parent.attachment);
    VfsPathHoldMount(mount);
    *out={mount,&directory->object};return 0;
}
static bool VfsReservedVirtualLocked(const VfsPath& parent,const char* name,VfsPath* out)
{
    for(const auto& mount:g_mounts) {
        if(!VfsMountPublished(mount))continue;
        VfsPath current=mount.covered;
        while(VfsVirtual(current)) {
            auto* directory=static_cast<VfsVirtualDirectory*>(current.object->priv);
            if(VfsPathSame(directory->parent,parent) && StrEq(directory->name,name)) {
                *out=current;VfsPathRetain(current);return true;
            }
            current=directory->parent;
        }
    }
    return false;
}
static int VfsEnterMountsLocked(VfsPath* path)
{
    // Edges always point into a new attachment, so traversal cannot cycle.
    for(uint32_t step=0;step<VFS_MAX_MOUNT_DEPTH;++step) {
        MountEntry* child=nullptr;
        for(auto& mount:g_mounts)
            if(VfsMountPublished(mount) && mount.linked && VfsPathSame(mount.covered,*path)) { child=&mount;break; }
        if(!child)return 0;
        VfsPath next{};int error=VfsMountRootLocked(child,&next);
        if(error)return error;
        VfsPathRelease(*path);*path=next;
    }
    return -40;
}
struct VfsRetainedProvider {
    bool realIds=false;
    void Retain(const VfsPath& path) { VfsPathRetain(path); }
    void Release(const VfsPath& path) { VfsPathRelease(path); }
    bool Same(const VfsPath& a,const VfsPath& b) { return VfsPathSame(a,b); }
    int Stat(const VfsPath& path,RetainedNodeInfo& out) {
        if(!path.object)return -9;
        VnodeStat stat{};
        int error=path.object->ops->stat?path.object->ops->stat(path.object,&stat):-95;
        out={stat.isDir,stat.isSymlink};return error;
    }
    int Search(const VfsPath& path) {
        return path.object->ops->access?path.object->ops->access(path.object,1,realIds):-95;
    }
    int Lookup(const VfsPath& parent,const char* name,VfsPath& out) {
        out={};
        if(VfsReservedVirtualLocked(parent,name,&out)) {
            // Reservation conflicts through ordinary VFS mutations are refused.
            // A backend-direct mutation must not silently hide a real child.
            VfsObject* conflict=nullptr;
            int error=parent.object->ops->lookup?parent.object->ops->lookup(parent.object,name,&conflict,realIds):-95;
            VfsObjectRelease(conflict);
            if(error!=-2) { VfsPathRelease(out);out={};return error?error:-16; }
        } else {
            if(!parent.object->ops->lookup)return -95;
            VfsObject* object=nullptr;
            int error=parent.object->ops->lookup(parent.object,name,&object,realIds);
            if(error || !object) { VfsObjectRelease(object);return error?error:-5; }
            auto* mount=static_cast<MountEntry*>(parent.attachment);
            VfsPathHoldMount(mount);
            out={mount,object};
        }
        const int error=VfsEnterMountsLocked(&out);
        if(error) { VfsPathRelease(out);out={}; }
        return error;
    }
    int Parent(const VfsPath& path,const VfsPath& root,VfsPath& out) {
        out={};VfsPath current=path;VfsPathRetain(current);
        for(uint32_t step=0;step<=VFS_MAX_MOUNT_DEPTH;++step) {
            if(Same(current,root)) { out=current;return 0; }
            auto* mount=static_cast<MountEntry*>(current.attachment);
            if(mount->rootIdentityKnown &&
               current.object->identity==mount->rootIdentity && current.object->ops==mount->rootObjectOps) {
                if(!VfsMountPublished(*mount)) { out=current;return 0; }
                if(mount->covered.object) {
                VfsPath next=mount->covered;VfsPathRetain(next);VfsPathRelease(current);current=next;continue;
                }
            }
            if(VfsVirtual(current)) {
                out=static_cast<VfsVirtualDirectory*>(current.object->priv)->parent;VfsPathRetain(out);
            } else {
                VfsObject* parent=nullptr;
                const int error=current.object->ops->parent?current.object->ops->parent(current.object,&parent,realIds):-95;
                if(error || !parent) { VfsObjectRelease(parent);VfsPathRelease(current);return error?error:-5; }
                VfsPathHoldMount(mount);out={mount,parent};
            }
            VfsPathRelease(current);return 0;
        }
        VfsPathRelease(current);return -40;
    }
    int FollowLink(const VfsPath& path,VfsPath& out) {
        out={};int result=path.object->ops->follow_link?path.object->ops->follow_link(path,&out):0;
        return result==1 && (!out.object || !out.attachment)?-5:result;
    }
    int Readlink(const VfsPath& path,char* out,uint32_t size) {
        return path.object->ops->readlink?path.object->ops->readlink(path.object,out,size):-95;
    }
};
static int VfsPrepareMountpointLocked(const char* text,VfsPath* out)
{
    *out={};VfsPath current{};int error=VfsMountRootLocked(g_rootMount,&current);
    if(error)return error;
    VfsRetainedProvider provider;
    const char* next=text+1;
    while(*next) {
        char name[256];uint32_t length=0;
        while(*next && *next!='/') { if(length+1>=sizeof(name)) { error=-36;break; }name[length++]=*next++; }
        name[length]=0;
        if(error)break;
        // Mount publication uses canonical directory names. Symlink/dot paths
        // are intentionally rejected rather than creating ambiguous edges.
        if(!length || StrEq(name,".") || StrEq(name,"..")) { error=-22;break; }
        RetainedNodeInfo info{};
        if((error=provider.Stat(current,info)))break;
        if(!info.isDir) { error=-20;break; }
        if((error=provider.Search(current)))break;
        VfsPath child{};error=provider.Lookup(current,name,child);
        if(error==-2)error=VfsMakeVirtualLocked(current,name,&child);
        if(error)break;
        VfsPathRelease(current);current=child;
        if((error=provider.Stat(current,info)))break;
        if(!info.isDir) { error=-20;break; }
        if(*next=='/' && !*++next) { error=-22;break; }
    }
    if(error) { VfsPathRelease(current);return error; }
    *out=current;return 0;
}
// Global-name reporting for cwd/proc/debug consumers. Lookup never calls this.
static int VfsBackendDirectoryLocked(const VfsPath& path,char* out,uint32_t capacity,bool anyObject=false)
{
    if(!out || capacity<2)return -36;
    out[0]=0;if(!path.object)return -9;
    char suffix[512];uint32_t position=sizeof(suffix)-1;suffix[position]=0;
    VfsPath current=path;
    for(uint32_t depth=0;VfsVirtual(current);++depth) {
        if(depth>=256)return -40;
        auto* directory=static_cast<VfsVirtualDirectory*>(current.object->priv);
        VfsPath visible{};
        bool linked=VfsReservedVirtualLocked(directory->parent,directory->name,&visible) && VfsPathSame(visible,current);
        VfsPathRelease(visible);if(!linked)return -2;
        const uint32_t length=StrLen(directory->name);
        if(position<length+1)return -36;
        position-=length;for(uint32_t i=0;i<length;++i)suffix[position+i]=directory->name[i];
        suffix[--position]='/';current=directory->parent;
    }
    auto report=current.object->ops->directory_path;
    if(anyObject) {
        VnodeStat stat{};if(int error=current.object->ops->stat(current.object,&stat))return error;
        if(current.object->ops->object_path)report=current.object->ops->object_path;
        else if(!stat.isDir)return -95;
    }
    if(!report)return -95;
    char relative[512];int error=report(current.object,relative,sizeof(relative));
    if(error)return error;
    if(relative[0]!='/')return -5;
    uint32_t written=StrLen(relative);const uint32_t tail=sizeof(suffix)-position-1;
    if(tail && written==1)written=0;
    if(written+tail>=capacity)return -36;
    if(written)StrCopy(out,relative,capacity);
    for(uint32_t i=0;i<=tail;++i)out[written+i]=suffix[position+i];
    return 0;
}
static int VfsGlobalDirectoryLocked(const VfsPath& path,char* out,uint32_t capacity,bool anyObject=false)
{
    if(!out || capacity<2)return -36;
    out[0]=0;if(!path.attachment || !path.object)return -9;
    auto* mount=static_cast<MountEntry*>(path.attachment);
    // A regular-file bind has no descendants. Its visible entry is retained
    // separately from the source inode, whose backend need not report names.
    // Reconstruct from that entry's live parent, never the source spelling.
    if(anyObject && mount->rootObject && mount->coveredParent.object &&
       path.object->identity==mount->rootIdentity && path.object->ops==mount->rootObjectOps) {
        int error=VfsGlobalDirectoryLocked(mount->coveredParent,out,capacity);
        return error?error:VfsAppendLeaf(out,capacity,mount->coveredName);
    }
    char relative[512];int error=VfsBackendDirectoryLocked(path,relative,sizeof(relative),anyObject);
    if(error)return error;
    if(mount->rootObject) {
        char boundRoot[512];auto* rootObject=mount->rootObject;
        if(!rootObject->ops->directory_path)return -95;
        if((error=rootObject->ops->directory_path(rootObject,boundRoot,sizeof(boundRoot))))return error;
        uint32_t prefixLength=StrLen(boundRoot);
        if(prefixLength>1) {
            for(uint32_t i=0;i<prefixLength;++i)if(relative[i]!=boundRoot[i])return -2;
            if(relative[prefixLength] && relative[prefixLength]!='/')return -2;
            if(!relative[prefixLength]) {relative[0]='/';relative[1]=0;}
            else {uint32_t i=0;do {relative[i]=relative[prefixLength+i];}while(relative[i++]);}
        }
    }
    const uint32_t prefix=StrLen(mount->mountPoint),length=StrLen(relative);
    const bool root=prefix==1 && mount->mountPoint[0]=='/';
    const uint32_t total=root?length:prefix+(length==1?0:length);
    if(total>=capacity)return -36;
    if(root)StrCopy(out,relative,capacity);
    else {
        for(uint32_t i=0;i<prefix;++i)out[i]=mount->mountPoint[i];
        if(length>1)for(uint32_t i=0;i<length;++i)out[prefix+i]=relative[i];
        out[total]=0;
    }
    return 0;
}
int VfsPathObjectPathFromCallback(const VfsPath& path,char* out,uint32_t capacity)
{
    return VfsGlobalDirectoryLocked(path,out,capacity,true);
}
int VfsPathDirectoryPathFromCallback(const VfsPath& path,char* out,uint32_t capacity)
{
    return VfsGlobalDirectoryLocked(path,out,capacity);
}
int VfsPathDirectoryPath(const VfsPath& path,char* out,uint32_t capacity)
{
    FileMutationGuard lookup(false);return VfsGlobalDirectoryLocked(path,out,capacity);
}
static int VfsPathFromVnodeLocked(Vnode* node,VfsPath* out)
{
    if(!out)return -22;
    *out={};if(!node)return -9;
    auto* mount=static_cast<MountEntry*>(node->mountOwner);
    if(!mount || !mount->backing)return -9;
    VfsObject* object=nullptr;int error=0;
    if(node->backendObject && node->backendObject->ops==&g_virtualDirectoryOps) {
        object=node->backendObject;VfsObjectRetain(object);
    } else if(mount->backing->fsOps->vnode_object)error=mount->backing->fsOps->vnode_object(node,&object);
    else error=-95;
    if(error || !object) { VfsObjectRelease(object);return error?error:-5; }
    VfsPathHoldMount(mount);*out={mount,object};return 0;
}
int VfsPathFromVnode(Vnode* node,VfsPath* out)
{
    FileMutationGuard lookup(false);return VfsPathFromVnodeLocked(node,out);
}
int VfsPathResolve(const VfsPath& root,const VfsPath& start,const char* text,VfsPath* out,bool followFinal,bool realIds)
{
    if(!out)return -22;
    *out={};if(!root.object || !start.object)return -9;
    FileMutationGuard lookup(false);VfsRetainedProvider provider{realIds};
    RetainedWalkResult<VfsPath> result{};
    const int error=WalkRetainedPath(root,start,text,&result,provider,{followFinal});
    if(error)return error;
    VfsPathRelease(result.parent);*out=result.object;return 0;
}
int VfsNullDeviceAnchorInitialize()
{
    FileMutationGuard transaction;
    if(g_nullDeviceAnchor.object)return -16;
    VfsPath root{};int error=VfsMountRootLocked(g_initialNamespace.root,&root);
    if(error)return error;
    VfsRetainedProvider provider;RetainedWalkResult<VfsPath> walk{};
    error=WalkRetainedPath(root,root,"/dev",&walk,provider);
    if(!error) {
        VnodeStat stat{};error=walk.object.object->ops->stat?walk.object.object->ops->stat(walk.object.object,&stat):-95;
        if(!error && (!stat.isDir || VfsVirtual(walk.object)))error=-20;
        if(!error) {g_nullDeviceAnchor=walk.object;walk.object={};}
    }
    VfsPathRelease(walk.parent);VfsPathRelease(walk.object);VfsPathRelease(root);return error;
}
int VfsNullDeviceAllowed(const VfsPath& root,const VfsPath& start,const char* text,bool* allowed,bool followFinal)
{
    if(!allowed)return -22;
    *allowed=false;if(!root.object || !start.object || !text)return -9;
    FileMutationGuard transaction(false);
    if(!g_nullDeviceAnchor.object)return 0;
    VfsRetainedProvider provider;RetainedWalkResult<VfsPath> walk{};
    int error=WalkRetainedPath(root,start,text,&walk,provider,{followFinal,true});
    if(!error && !walk.object.object && walk.parent.object && !walk.trailing && StrEq(walk.leaf,"null")) {
        auto* parent=static_cast<MountEntry*>(walk.parent.attachment);
        auto* anchor=static_cast<MountEntry*>(g_nullDeviceAnchor.attachment);
        if(parent->backing==anchor->backing && walk.parent.object->ops==g_nullDeviceAnchor.object->ops &&
           walk.parent.object->identity==g_nullDeviceAnchor.object->identity) {
            // Identity and the deepest applicable NODEV flag share this lookup
            // transaction. No reported name is used for later filesystem I/O.
            if(VfsMountAttributes(parent)&4)error=-13;
            else *allowed=true;
        }
    }
    VfsPathRelease(walk.parent);VfsPathRelease(walk.object);
    return error==-2?0:error;
}
int VfsPathBrokerAllowed(const VfsPath& root,const char* absolute,bool* allowed)
{
    if(!allowed)return -22;
    *allowed=false;if(!root.attachment || !root.object || !absolute || absolute[0]!='/')return -22;
    FileMutationGuard lookup(false);auto* rootMount=static_cast<MountEntry*>(root.attachment);
    if(!VfsMountPublished(*rootMount) || rootMount->rootObject)return 0;
    bool hasBind=false;
    for(const auto& mount:g_mounts)
        if(VfsMountPublished(mount) && mount.owner==rootMount->owner && mount.rootObject) {hasBind=true;break;}
    if(!hasBind) {*allowed=true;return 0;} // unchanged initial/copied topology
    char prefix[512];uint32_t length=StrLen(absolute);if(length>=sizeof(prefix))return -36;
    StrCopy(prefix,absolute,sizeof(prefix));
    // Broker files can have entirely synthetic tails. Walk each existing
    // prefix by identity; only stop at a genuinely absent component. No name
    // is used to dispatch an ordinary filesystem operation afterward.
    for(uint32_t end=1;end<=length;++end) {
        if(prefix[end]!='/' && prefix[end])continue;
        char saved=prefix[end];prefix[end]=0;
        VfsRetainedProvider provider;RetainedWalkResult<VfsPath> result{};
        int error=WalkRetainedPath(root,root,prefix,&result,provider);
        prefix[end]=saved;
        if(error) {
            if(error==-2 || (!saved && error==-95)) {*allowed=true;return 0;}
            return error;
        }
        bool bound=static_cast<MountEntry*>(result.object.attachment)->rootObject!=nullptr;
        VfsPathRelease(result.object);VfsPathRelease(result.parent);
        if(bound)return 0;
    }
    *allowed=true;return 0;
}
int VfsPathStat(const VfsPath& path,VnodeStat* out)
{
    if(!out)return -22;
    *out={};if(!path.object)return -9;
    FileMutationGuard lookup(false);
    return path.object->ops->stat?path.object->ops->stat(path.object,out):-95;
}
int VfsPathDeviceActivation(const VfsPath& root,const char* absolute)
{
    if(!root.object || !root.attachment || !absolute || absolute[0]!='/')return -22;
    FileMutationGuard lookup(false);
    // Synthetic device tails may have no backing inode. The last existing
    // identity selects the applicable mount; a real submount has its own flags.
    auto* mount=static_cast<MountEntry*>(root.attachment);
    if(!VfsMountPublished(*mount))return -2;
    uint64_t attributes=mount->attributes;
    char prefix[512];uint32_t length=StrLen(absolute);if(length>=sizeof(prefix))return -36;
    StrCopy(prefix,absolute,sizeof(prefix));
    for(uint32_t end=1;end<=length;++end) {
        if(prefix[end]!='/' && prefix[end])continue;
        char saved=prefix[end];prefix[end]=0;
        VfsRetainedProvider provider;RetainedWalkResult<VfsPath> result{};
        int error=WalkRetainedPath(root,root,prefix,&result,provider);
        prefix[end]=saved;
        if(error) {
            if(error==-2 || (!saved && error==-95))return attributes&4?-13:0;
            return error;
        }
        attributes=static_cast<MountEntry*>(result.object.attachment)->attributes;
        VfsPathRelease(result.object);VfsPathRelease(result.parent);
    }
    return attributes&4?-13:0;
}
int VfsPathAccess(const VfsPath& path,uint8_t required,bool realIds)
{
    if(required&~7u)return -22;
    if(!path.object)return -9;
    FileMutationGuard lookup(false);
    if((required&2) && VfsReadOnly(static_cast<MountEntry*>(path.attachment))) {
        VnodeStat stat{};int error=path.object->ops->stat?path.object->ops->stat(path.object,&stat):-95;
        if(error)return error;
        if((stat.mode&0170000)!=0140000)return -30;
    }
    return path.object->ops->access?path.object->ops->access(path.object,required,realIds):-95;
}
static Vnode* VfsPathOpenLocked(const VfsPath& path,int flags,int* error)
{
    int localError=0;if(!error)error=&localError;*error=0;
    if(!path.object || !path.attachment) { *error=-9;return nullptr; }
    if((flags & (VFS_O_CREATE|VFS_O_EXCL)) ||
       ((flags&VFS_O_METADATA) && (flags&~VFS_O_METADATA)) ||
       ((flags&VFS_O_READWRITE) && !(flags&VFS_O_WRITE)) ||
       ((flags&VFS_O_EXEC) && (flags&(VFS_O_WRITE|VFS_O_READWRITE|VFS_O_TRUNC|VFS_O_APPEND)))) {
        *error=-22;return nullptr;
    }
    if(!path.object->ops->open) { *error=-95;return nullptr; }
    auto* mount=static_cast<MountEntry*>(path.attachment);
    if(VfsReadOnly(mount) && (flags&(VFS_O_WRITE|VFS_O_TRUNC))) { *error=-30;return nullptr; }
    if((mount->attributes&4) && !(flags&VFS_O_METADATA)) {
        VnodeStat stat{};*error=path.object->ops->stat?path.object->ops->stat(path.object,&stat):-95;
        if(*error)return nullptr;
        if(VfsNodevDenied(mount,stat,flags)) {*error=-13;return nullptr;}
    }
    int openFlags=flags&~VFS_O_TRUNC;
    if(flags&VFS_O_TRUNC) { if(!(openFlags&VFS_O_WRITE))openFlags|=VFS_O_READWRITE;openFlags|=VFS_O_WRITE; }
    Vnode* node=path.object->ops->open(path.object,openFlags,error);
    if(!node) { if(!*error)*error=-5;return nullptr; }
    // Every retained descriptor exposes this exact identity to proc reporting,
    // including O_PATH regular files. Acquiring it under an FD lock must need
    // no backend callback or vnode close inside proc lookup.
    if(!node->backendObject) {
        node->backendObject=path.object;VfsObjectRetain(path.object);
    }
    // Ownership transfer occurs while the borrowed path still pins backing.
    VfsAttachMount(node,static_cast<MountEntry*>(path.attachment),flags);
    if(flags&VFS_O_TRUNC) {
        *error=VfsTruncateLocked(node,0);
        if(*error) { VfsClose(node);return nullptr; }
    }
    return node;
}

Vnode* VfsPathOpen(const VfsPath& path,int flags,int* error)
{
    FileMutationGuard lookup(flags&VFS_O_TRUNC);return VfsPathOpenLocked(path,flags,error);
}

// Compatibility callers still use pathname mutation APIs. Reserve all live
// mountpoint objects and virtual ancestor names in the same transaction until
// those callers migrate to the retained parent-operation interface.
static int VfsReservedMutationLocked(const char* text,bool rejectVirtualChildren=false)
{
    if(!text || text[0]!='/' || !g_rootMount || !g_rootMount->rootIdentityKnown)return 0;
    VfsPath root{};int error=VfsMountRootLocked(g_rootMount,&root);
    if(error)return error;
    VfsRetainedProvider provider;
    RetainedWalkResult<VfsPath> result{};
    error=WalkRetainedPath(root,root,text,&result,provider,{false,true,true});
    VfsPathRelease(root);
    if(error)return error==-95?0:error; // Old unsupported backends keep old API dispatch.
    if(!result.parent.object) { VfsPathRelease(result.object);return -16; }
    VfsPath reserved{};
    bool busy=VfsReservedVirtualLocked(result.parent,result.leaf,&reserved);
    VfsPathRelease(reserved);
    int failure=busy?-16:0;
    if(!busy && result.parent.object->ops->lookup) {
        VfsObject* child=nullptr;
        int found=result.parent.object->ops->lookup(result.parent.object,result.leaf,&child,false);
        if(!found && child)for(const auto& mount:g_mounts) {
            if(!VfsMountPublished(mount))continue;
            VfsPath identity{result.parent.attachment,child};
            if(VfsPathSame(mount.covered,identity))failure=-16;
            if(rejectVirtualChildren && !failure) {
                VfsPath current=mount.covered;
                while(VfsVirtual(current)) {
                    auto* directory=static_cast<VfsVirtualDirectory*>(current.object->priv);
                    if(VfsPathSame(directory->parent,identity)) { failure=-39;break; }
                    current=directory->parent;
                }
            }
        }
        VfsObjectRelease(child);
    }
    VfsPathRelease(result.parent);VfsPathRelease(result.object);
    return failure;
}

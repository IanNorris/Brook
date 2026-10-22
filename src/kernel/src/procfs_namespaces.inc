// Included before retained proc objects. Only actual mount namespaces are
// exposed. Namespace links/directories pin task identity; followed objects own
// active topology leases independently of tasks, paths and filesystem snapshots.
static int ProcAcquireMountNamespace(uint16_t pid,uint64_t incarnation,uint16_t tgid,
                                      uint64_t group,MountNamespace** out)
{
    *out=nullptr;auto* task=SchedulerFindProcessReference(pid);if(!task)return -2;
    int error=0;
    if(task->incarnation!=incarnation || task->tgid!=tgid ||
       (task->isThread?task->leaderIncarnation:task->incarnation)!=group ||
       !__atomic_load_n(&task->mountNamespaceMember,__ATOMIC_ACQUIRE))error=-2;
    auto* self=ProcessCurrent();
    // Deliberately narrower than general Linux ptrace access. Same thread
    // group or real CAP_SYS_PTRACE authority in the target's user namespace
    // suffices. Never assume dumpability from an unimplemented prctl option.
    if(!error) {
        bool same=self && self->tgid==tgid &&
            (self->isThread?self->leaderIncarnation:self->incarnation)==group;
        auto* owner=ProcessUserNamespaceAcquire(task);
        bool allowed=same || ProcessCapable(self,owner,19); // CAP_SYS_PTRACE
        UserNamespaceRelease(owner);if(!allowed)error=-13;
    }
    FsSnapshot fs{};
    if(!error)error=ProcessFsSnapshot(task,&fs);
    if(!error && !VfsMountNamespaceHandleAcquire(fs.mountNamespace))error=-2;
    if(!error)*out=fs.mountNamespace; // active lease owns separate storage
    FsSnapshotRelease(&fs);ProcessUnref(task);return error;
}
struct ProcMountNamespaceObject { VfsObject object;MountNamespace* ns; };
static void ProcNamespaceDestroy(VfsObject* object)
{
    auto* value=static_cast<ProcMountNamespaceObject*>(object->priv);
    auto* ns=value->ns;kfree(value);VfsMountNamespaceHandleRelease(ns);
}
static int ProcNamespaceStat(VfsObject* object,VnodeStat* out)
{
    if(!out)return -22;
    *out={};out->mode=0100444;out->ino=object->identity;out->dev=0x6e736673;out->nlink=1;return 0;
}
static int ProcNamespaceAccess(VfsObject*,uint8_t required,bool) { return required&3?-13:0; }
static int ProcNamespaceNodeStat(Vnode* node,VnodeStat* out) { return ProcNamespaceStat(node->backendObject,out); }
static int ProcNamespaceRead(Vnode*,void*,uint64_t,uint64_t*) { return -22; }
static VnodeOps g_procNamespaceNodeOps={ProcFileOpen,ProcNamespaceRead,ProcFileWrite,nullptr,ProcFileClose,ProcNamespaceNodeStat,nullptr};
static Vnode* ProcNamespaceOpen(VfsObject* object,int flags,int* error)
{
    *error=flags&(VFS_O_WRITE|VFS_O_READWRITE)?-1:flags&VFS_O_EXEC?-13:flags&~VFS_O_METADATA?-22:0;
    if(*error)return nullptr;
    auto* node=MakeProcVnode(nullptr,0,false,&g_procNamespaceNodeOps);
    if(!node) { *error=-12;return nullptr; }
    node->type=VnodeType::Namespace;node->backendObject=object;VfsObjectRetain(object);return node;
}
static const VfsObjectOps g_procNamespaceOps={
    .destroy=ProcNamespaceDestroy,.stat=ProcNamespaceStat,.access=ProcNamespaceAccess,
    .lookup=nullptr,.parent=nullptr,.readlink=nullptr,.open=ProcNamespaceOpen,
};
static int ProcNamespaceObject(uint16_t pid,uint64_t incarnation,uint16_t tgid,uint64_t group,VfsObject** out)
{
    *out=nullptr;MountNamespace* ns=nullptr;
    if(int error=ProcAcquireMountNamespace(pid,incarnation,tgid,group,&ns))return error;
    auto* value=static_cast<ProcMountNamespaceObject*>(kmalloc(sizeof(ProcMountNamespaceObject)));
    if(!value) {VfsMountNamespaceHandleRelease(ns);return -12;}
    value->ns=ns;value->object={&g_procNamespaceOps,value,VfsMountNamespaceIdentity(ns),1};*out=&value->object;return 0;
}
static int ProcNamespaceReadlink(uint16_t pid,uint64_t incarnation,uint16_t tgid,uint64_t group,char* out,uint32_t capacity)
{
    MountNamespace* ns=nullptr;if(int error=ProcAcquireMountNamespace(pid,incarnation,tgid,group,&ns))return error;
    char text[32];char* end=AppendStr(text,"mnt:[");end=U64ToDec(end,VfsMountNamespaceIdentity(ns));*end++=']';
    uint32_t bytes=end-text;if(bytes>capacity)bytes=capacity;memcpy(out,text,bytes);
    VfsMountNamespaceHandleRelease(ns);return bytes;
}
static int ProcNsDirectoryReaddir(Vnode* node,DirEntry* out,uint32_t* cookie)
{
    auto* value=static_cast<ProcPriv*>(node->priv);ProcessSnapshot snapshot{};
    if(!SchedulerSnapshotProcess(value->pid,&snapshot) || snapshot.incarnation!=value->incarnation ||
       snapshot.tgid!=value->tgid || snapshot.groupIncarnation!=value->groupIncarnation ||
       snapshot.state==ProcessState::Terminated)return -2;
    if(*cookie)return 0;
    *out={};ProcStrCopy(out->name,"mnt",sizeof(out->name));++*cookie;return 1;
}
static int ProcNsDirectoryStat(Vnode* node,VnodeStat* out)
{
    if(node->backendObject)return node->backendObject->ops->stat(node->backendObject,out);
    *out={};out->isDir=true;out->mode=0040555;out->dev=0x70726f63;out->nlink=2;return 0;
}
static VnodeOps g_procNsDirectoryOps={ProcFileOpen,ProcDirRead,ProcFileWrite,ProcNsDirectoryReaddir,ProcDirClose,ProcNsDirectoryStat,nullptr};
static Vnode* MakeProcNsDirectory(const ProcessSnapshot& snapshot)
{
    auto* node=MakeProcVnode(nullptr,0,true,&g_procNsDirectoryOps);if(!node)return nullptr;
    auto* value=static_cast<ProcPriv*>(node->priv);value->pid=snapshot.pid;value->tgid=snapshot.tgid;
    value->incarnation=snapshot.incarnation;value->groupIncarnation=snapshot.groupIncarnation;return node;
}
static int ProcMetadataStat(Vnode* node,VnodeStat* out) { return node->backendObject->ops->stat(node->backendObject,out); }
static VnodeOps g_procMetadataOps={ProcFileOpen,ProcFileRead,ProcFileWrite,nullptr,ProcFileClose,ProcMetadataStat,nullptr};

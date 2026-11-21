// Included before retained proc objects. A namespace link resolves against the
// captured task incarnation. A followed mount namespace owns a topology lease;
// the other descriptors expose immutable namespace identity and type.
enum class ProcNamespaceKind : uint8_t { Mount, User, Pid, Net };
static constexpr const char* ProcNamespaceNames[]{"mnt", "user", "pid", "net"};
static constexpr uint32_t ProcNamespaceTypes[]{0x00020000, 0x10000000, CloneNewPid, CloneNewNet};

static int ProcAcquireNamespaceTask(uint16_t pid,uint64_t incarnation,uint16_t tgid,
                                    uint64_t group,Process** out)
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
    if(error)ProcessUnref(task);else *out=task;
    return error;
}
struct ProcNamespaceDescriptor {
    VfsObject object;
    MountNamespace* mount;
    ProcNamespaceKind kind;
};
static int ProcReadNamespaceIdentity(Process* task, ProcNamespaceKind kind, uint64_t& identity,
                                     MountNamespace*& mount)
{
    switch(kind) {
        case ProcNamespaceKind::Mount: {
            FsSnapshot fs{};
            int error=ProcessFsSnapshot(task,&fs);
            if(!error && !VfsMountNamespaceHandleAcquire(fs.mountNamespace))error=-2;
            if(!error) { mount=fs.mountNamespace;identity=VfsMountNamespaceIdentity(mount); }
            FsSnapshotRelease(&fs);return error;
        }
        case ProcNamespaceKind::User: {
            auto* scope=ProcessUserNamespaceAcquire(task);
            identity=scope?scope->identity:0;
            UserNamespaceRelease(scope);
            break;
        }
        case ProcNamespaceKind::Pid:
            identity=PidNamespaceTag(ProcessPidView(task));
            break;
        case ProcNamespaceKind::Net:
            identity=ProcessNetworkScope(task);
            break;
    }
    // Keep distinct namespace types in separate inode-number ranges even
    // when their allocation counters have the same value.
    if(identity >= (uint64_t{1} << 62))return -75;
    identity |= static_cast<uint64_t>(kind) << 62;
    return 0;
}
static void ProcNamespaceDestroy(VfsObject* object)
{
    auto* value=static_cast<ProcNamespaceDescriptor*>(object->priv);
    auto* mount=value->mount;kfree(value);VfsMountNamespaceHandleRelease(mount);
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
int ProcFsOpenedNamespaceType(const Vnode* node)
{
    if(!node || !node->backendObject || node->backendObject->ops!=&g_procNamespaceOps)return -25;
    auto* value=static_cast<const ProcNamespaceDescriptor*>(node->backendObject->priv);
    return ProcNamespaceTypes[static_cast<unsigned>(value->kind)];
}

static int ProcNamespaceObject(uint16_t pid,uint64_t incarnation,uint16_t tgid,uint64_t group,
                               ProcNamespaceKind kind,VfsObject** out)
{
    *out=nullptr;Process* task=nullptr;
    if(int error=ProcAcquireNamespaceTask(pid,incarnation,tgid,group,&task))return error;
    MountNamespace* mount=nullptr;uint64_t identity=0;
    const int error=ProcReadNamespaceIdentity(task,kind,identity,mount);
    ProcessUnref(task);if(error)return error;
    auto* value=static_cast<ProcNamespaceDescriptor*>(kmalloc(sizeof(ProcNamespaceDescriptor)));
    if(!value) {VfsMountNamespaceHandleRelease(mount);return -12;}
    *value={{&g_procNamespaceOps,value,identity,1},mount,kind};
    *out=&value->object;return 0;
}
static int ProcNamespaceReadlink(uint16_t pid,uint64_t incarnation,uint16_t tgid,uint64_t group,
                                 ProcNamespaceKind kind,char* out,uint32_t capacity)
{
    VfsObject* target=nullptr;
    if(int error=ProcNamespaceObject(pid,incarnation,tgid,group,kind,&target))return error;
    char text[32];char* end=AppendStr(text,ProcNamespaceNames[static_cast<unsigned>(kind)]);
    end=AppendStr(end,":[");end=U64ToDec(end,target->identity);*end++=']';
    uint32_t bytes=end-text;if(bytes>capacity)bytes=capacity;memcpy(out,text,bytes);
    VfsObjectRelease(target);return bytes;
}
static int ProcNsDirectoryReaddir(Vnode* node,DirEntry* out,uint32_t* cookie)
{
    auto* value=static_cast<ProcPriv*>(node->priv);ProcessSnapshot snapshot{};
    if(!SchedulerSnapshotProcess(value->pid,&snapshot) || snapshot.incarnation!=value->incarnation ||
       snapshot.tgid!=value->tgid || snapshot.groupIncarnation!=value->groupIncarnation ||
       snapshot.state==ProcessState::Terminated)return -2;
    if(*cookie>=sizeof(ProcNamespaceNames)/sizeof(*ProcNamespaceNames))return 0;
    *out={};ProcStrCopy(out->name,ProcNamespaceNames[*cookie],sizeof(out->name));++*cookie;return 1;
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

// Retained procfs directory identities. A PID number alone is never authority:
// every per-task operation checks the captured incarnation. No Process strong
// reference is kept (cwd=/proc/self must not create a Process ownership cycle).
static constexpr uint32_t PROC_PID_EXE=5,PROC_PID_CWD=6,PROC_PID_ROOT=8;
static_assert(NUM_PID_ENTRIES==15,"Update retained proc PID entry indices when changing the catalog");
enum class ProcObjectKind : uint8_t { Root,Net,Pid,Task,Fds,GlobalFile,NetFile,PidFile,FdLink,Self,ThreadSelf,UserMap,Sys,SysKernel,OverflowId,NsDirectory,NsLink,FdInfoDir,FdInfoFile };
struct ProcObject {
    VfsObject object;
    VfsObject* parent;
    ProcObject* next;
    ProcObjectKind kind;
    uint16_t pid,tgid;
    uint64_t incarnation,group;
    bool taskMember;
    uint32_t index;
    uint32_t viewTid;
    const PidNamespace* pidView;
    UserNamespace* userNamespace;
};
static SpinLock g_procObjectLock;
static ProcObject* g_procObjects;
static uint64_t g_procObjectIdentity=2;
static const VfsObjectOps* ProcObjectOperations();
static bool ProcObjectDirectory(ProcObjectKind kind)
{
    return kind<=ProcObjectKind::Fds || kind==ProcObjectKind::Sys || kind==ProcObjectKind::SysKernel ||
           kind==ProcObjectKind::NsDirectory || kind==ProcObjectKind::FdInfoDir;
}
static int ProcObjectSnapshot(const ProcObject* object,ProcessSnapshot* snapshot)
{
    if(object->kind==ProcObjectKind::UserMap) {*snapshot={};return 0;}
    if(!object->pid) { *snapshot={};return 0; }
    const bool found = object->pidView
        ? SchedulerSnapshotProcView(object->pidView, object->viewTid, 0, 0, 0, false, snapshot)
        : SchedulerSnapshotProcess(object->pid, snapshot);
    if(!found || snapshot->pid!=object->pid || snapshot->incarnation!=object->incarnation ||
       snapshot->tgid!=object->tgid || snapshot->groupIncarnation!=object->group ||
       (object->taskMember && snapshot->state==ProcessState::Terminated))return -2;
    return 0;
}
static int ProcObjectMake(ProcObjectKind kind,VfsObject* parent,const ProcessSnapshot* snapshot,
                           uint32_t index,bool taskMember,VfsObject** out,const PidNamespace* rootView=nullptr)
{
    *out=nullptr;auto* value=static_cast<ProcObject*>(kmalloc(sizeof(ProcObject)));
    if(!value)return -12;
    *value={};value->kind=kind;value->parent=parent;VfsObjectRetain(parent);
    value->index=index;value->taskMember=taskMember;
    value->pidView = parent ? static_cast<ProcObject*>(parent->priv)->pidView : rootView;
    if (!parent) PidNamespaceRetain(value->pidView);
    if(snapshot) {
        value->pid=snapshot->pid;value->tgid=snapshot->tgid;value->viewTid=snapshot->viewTid;
        value->incarnation=snapshot->incarnation;value->group=snapshot->groupIncarnation;
    }
    if(kind==ProcObjectKind::UserMap) {
        auto* task=SchedulerFindProcessReference(value->pid);
        if(!task || task->incarnation!=value->incarnation) {
            if(task)ProcessUnref(task);VfsObjectRelease(value->parent);kfree(value);return -2;
        }
        value->userNamespace=ProcessUserNamespaceAcquire(task);ProcessUnref(task);
    }
    SpinLockAcquire(&g_procObjectLock);
    for(auto* entry=g_procObjects;entry;entry=entry->next) {
        if(entry->kind==kind && entry->pid==value->pid && entry->incarnation==value->incarnation &&
           entry->tgid==value->tgid && entry->group==value->group && entry->index==index &&
           entry->taskMember==taskMember && entry->pidView==value->pidView &&
           ((!entry->parent && !parent) || (entry->parent && parent && entry->parent->ops==parent->ops && entry->parent->identity==parent->identity)) &&
           VfsObjectTryRetain(&entry->object)) {
            *out=&entry->object;SpinLockRelease(&g_procObjectLock);
            UserNamespaceRelease(value->userNamespace);
            if (!parent) PidNamespaceRelease(value->pidView);
            VfsObjectRelease(value->parent);kfree(value);return 0;
        }
    }
    // Only root has a fixed identity across zero-reference intervals. Other
    // live keys are interned, using non-reused checked IDs rather than a lossy
    // hash of PID/incarnation/file index.
    if(g_procObjectIdentity>=(uint64_t{1}<<62))__builtin_trap();
    uint64_t identity=kind==ProcObjectKind::Root && !rootView ? 1 : g_procObjectIdentity++;
    value->object={ProcObjectOperations(),value,identity,1};
    value->next=g_procObjects;g_procObjects=value;*out=&value->object;
    SpinLockRelease(&g_procObjectLock);return 0;
}
static void ProcObjectDestroy(VfsObject* object)
{
    auto* value=static_cast<ProcObject*>(object->priv);VfsObject* parent=value->parent;
    SpinLockAcquire(&g_procObjectLock);
    ProcObject** entry=&g_procObjects;while(*entry && *entry!=value)entry=&(*entry)->next;
    if(*entry!=value)__builtin_trap();
    *entry=value->next;SpinLockRelease(&g_procObjectLock);
    UserNamespaceRelease(value->userNamespace);
    if (!parent) PidNamespaceRelease(value->pidView);
    kfree(value);VfsObjectRelease(parent);
}
static int ProcReportedInode(VfsObject* object,uint64_t* out)
{
    static_assert(MAX_FDS<=ProcInodeIndexes,"Expand proc inode index domain with the descriptor limit");
    static_assert(static_cast<uint32_t>(ProcObjectKind::FdInfoFile)<ProcInodeRoles);
    auto* value=static_cast<ProcObject*>(object->priv);
    return ProcInodeNumber(value->incarnation,static_cast<uint32_t>(value->kind),
                           value->taskMember,value->index,*out)?0:-75; // EOVERFLOW
}
static int ProcObjectStat(VfsObject* object,VnodeStat* out)
{
    if(!out)return -22;
    auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if(value->kind!=ProcObjectKind::NsDirectory && value->kind!=ProcObjectKind::NsLink) {
        const int error=ProcObjectSnapshot(value,&snapshot);
        // A held fdinfo directory may be a chroot. It stays a directory
        // after the original task exits, but lookups never attach to a
        // reused PID. The detached directory has no children.
        if(error && (error!=-2 || value->kind!=ProcObjectKind::FdInfoDir))return error;
        if(error)snapshot={};
    }
    if(value->kind==ProcObjectKind::FdLink || value->kind==ProcObjectKind::FdInfoFile) {
        FdSnapshot fd{};if(!SchedulerGetFdInfo(value->pid,value->index,&fd,value->incarnation))return -2;
    }
    *out={};out->isDir=ProcObjectDirectory(value->kind);
    out->isSymlink=value->kind==ProcObjectKind::NsLink || value->kind==ProcObjectKind::FdLink || value->kind==ProcObjectKind::Self ||
                   value->kind==ProcObjectKind::ThreadSelf ||
                   (value->kind==ProcObjectKind::PidFile && (value->index==PROC_PID_EXE || value->index==PROC_PID_CWD || value->index==PROC_PID_ROOT));
    out->mode=out->isDir?0040555:out->isSymlink?0120777:0100444;
    if(value->pid && value->kind!=ProcObjectKind::UserMap &&
       value->kind!=ProcObjectKind::NsDirectory && value->kind!=ProcObjectKind::NsLink) {
        // Process entries belong to the effective owner, like their status
        // credentials. Syscall stat conversion maps these canonical IDs.
        out->uid=snapshot.euid;out->gid=snapshot.egid;
    }
    if(value->kind==ProcObjectKind::UserMap) {out->mode=0100644;out->uid=value->userNamespace?value->userNamespace->owner:0;}
    if(int error=ProcReportedInode(object,&out->ino))return error;out->dev=0x70726f63;
    // Chromium uses the task-directory link count to detect a single thread.
    // Retained object lookup must match the live legacy proc directory stat.
    out->nlink=value->kind==ProcObjectKind::Task ? 2+snapshot.threadCount : out->isDir?2:1;
    return 0;
}
static int ProcOpenedIdentity(Vnode* node,VnodeStat* out)
{
    auto* object=node->backendObject;
    auto* value=static_cast<ProcObject*>(object->priv);
    auto* opened=static_cast<ProcPriv*>(node->priv);
    VnodeStat identity{};
    if(int error=object->ops->stat(object,&identity)) {
        // Ordinary proc files remain stat-able after their task is reaped.
        // Keep this opened incarnation's last owner; never adopt a reused PID.
        if(error!=-2 || value->kind!=ProcObjectKind::PidFile)return error;
        uint64_t owner=__atomic_load_n(&opened->reportedOwner,__ATOMIC_ACQUIRE);
        out->uid=uint32_t(owner>>32);out->gid=uint32_t(owner);
        return ProcReportedInode(object,&out->ino);
    }
    if(value->kind==ProcObjectKind::PidFile)
        __atomic_store_n(&opened->reportedOwner,(uint64_t{identity.uid}<<32)|identity.gid,__ATOMIC_RELEASE);
    // Keep generated file sizes and directory-specific metadata, while path
    // and opened-descriptor stats share validated identity and ownership.
    out->ino=identity.ino;out->uid=identity.uid;out->gid=identity.gid;
    return 0;
}
static int ProcObjectAccess(VfsObject* object,uint8_t required,bool realIds)
{
    VnodeStat stat{};if(int error=ProcObjectStat(object,&stat))return error;
    auto* value=static_cast<ProcObject*>(object->priv);
    if(value->kind==ProcObjectKind::UserMap)
        return ProcessCheckFileAccess(ProcessCurrent(),stat.uid,stat.gid,stat.mode,required,realIds);
    if(required&2)return -13;
    return (required&1) && !stat.isDir?-13:0;
}
static int ProcObjectLookup(VfsObject* object,const char* name,VfsObject** out,bool=false)
{
    *out=nullptr;auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if(int error=ProcObjectSnapshot(value,&snapshot))return error;
    if(!name || !name[0] || strchr(name,'/'))return -22;
    if(value->kind==ProcObjectKind::Root) {
        if(StrEqProc(name,"self"))return ProcObjectMake(ProcObjectKind::Self,object,nullptr,0,false,out);
        if(StrEqProc(name,"thread-self"))return ProcObjectMake(ProcObjectKind::ThreadSelf,object,nullptr,0,false,out);
        if(StrEqProc(name,"sys"))return ProcObjectMake(ProcObjectKind::Sys,object,nullptr,0,false,out);
        if(StrEqProc(name,"net"))return ProcObjectMake(ProcObjectKind::Net,object,nullptr,0,false,out);
        for(uint32_t i=0;i<NUM_GLOBAL;++i)
            if(StrEqProc(name,g_globalEntries[i].name))return ProcObjectMake(ProcObjectKind::GlobalFile,object,nullptr,i,false,out);
        const uint32_t limit = value->pidView ? 0x3fffffu : 65535u;
        const char* end=nullptr;uint32_t pid=ParseUint(name,&end,limit);
        if(!IsDigit(name[0]) || !end || *end || !pid || pid>limit)return -2;
        if (value->pidView) {
            if (!SchedulerSnapshotProcView(value->pidView, pid, 0, 0, 0, false, &snapshot)) return -2;
        } else if (!FindProcess(pid, &snapshot)) return -2;
        return ProcObjectMake(ProcObjectKind::Pid,object,&snapshot,0,false,out);
    }
    if(value->kind==ProcObjectKind::Sys) {
        return StrEqProc(name,"kernel")?ProcObjectMake(ProcObjectKind::SysKernel,object,nullptr,0,false,out):-2;
    }
    if(value->kind==ProcObjectKind::SysKernel) {
        if(StrEqProc(name,"overflowuid") || StrEqProc(name,"overflowgid"))
            return ProcObjectMake(ProcObjectKind::OverflowId,object,nullptr,StrEqProc(name,"overflowgid"),false,out);
        return -2;
    }
    if(value->kind==ProcObjectKind::Net) {
        static const char* names[]={"dev","tcp","udp","sockstat"};
        for(uint32_t i=0;i<4;++i)if(StrEqProc(name,names[i]))return ProcObjectMake(ProcObjectKind::NetFile,object,nullptr,i,false,out);
        return -2;
    }
    if(value->kind==ProcObjectKind::NsDirectory) {
        if(snapshot.state==ProcessState::Terminated)return -2;
        for(uint32_t i=0;i<sizeof(ProcNamespaceNames)/sizeof(*ProcNamespaceNames);++i)
            if(StrEqProc(name,ProcNamespaceNames[i]))
                return ProcObjectMake(ProcObjectKind::NsLink,object,&snapshot,i,value->taskMember,out);
        return -2;
    }
    if(value->kind==ProcObjectKind::Pid) {
        if(StrEqProc(name,"ns"))return ProcObjectMake(ProcObjectKind::NsDirectory,object,&snapshot,0,value->taskMember,out);
        if(auto kind=ProcMapKind(name))return ProcObjectMake(ProcObjectKind::UserMap,object,&snapshot,kind,value->taskMember,out);
        if(StrEqProc(name,"fd"))return ProcObjectMake(ProcObjectKind::Fds,object,&snapshot,0,value->taskMember,out);
        if(StrEqProc(name,"fdinfo"))return ProcObjectMake(ProcObjectKind::FdInfoDir,object,&snapshot,0,value->taskMember,out);
        if(!value->taskMember && StrEqProc(name,"task"))return ProcObjectMake(ProcObjectKind::Task,object,&snapshot,0,false,out);
        for(uint32_t i=0;i<NUM_PID_ENTRIES;++i)
            if(StrEqProc(name,g_pidEntries[i].name))return ProcObjectMake(ProcObjectKind::PidFile,object,&snapshot,i,value->taskMember,out);
        return -2;
    }
    const uint32_t limit = value->pidView && value->kind==ProcObjectKind::Task ? 0x3fffffu : 65535u;
    const char* end=nullptr;uint32_t number=ParseUint(name,&end,limit);
    if(!IsDigit(name[0]) || !end || *end || number>limit)return -2;
    if(value->kind==ProcObjectKind::Task) {
        if (!number) return -2;
        if (value->pidView) {
            if (!SchedulerSnapshotProcView(value->pidView, number, 0, value->tgid, value->group,
                                          false, &snapshot)) return -2;
        } else if (!SchedulerSnapshotTask(value->tgid, value->group, number, 0, &snapshot)) return -2;
        return ProcObjectMake(ProcObjectKind::Pid,object,&snapshot,0,true,out);
    }
    if(value->kind==ProcObjectKind::Fds || value->kind==ProcObjectKind::FdInfoDir) {
        if(value->kind==ProcObjectKind::FdInfoDir) {
            FsSnapshot actor{};
            const int error=ProcAcquireFsTarget(value->pid,value->incarnation,value->tgid,value->group,&actor);
            FsSnapshotRelease(&actor);
            if(error)return error;
        }
        FdSnapshot fd{};if(number>=MAX_FDS || !SchedulerGetFdInfo(value->pid,number,&fd,value->incarnation))return -2;
        const auto kind=value->kind==ProcObjectKind::Fds?ProcObjectKind::FdLink:ProcObjectKind::FdInfoFile;
        return ProcObjectMake(kind,object,&snapshot,number,value->taskMember,out);
    }
    return -20;
}
static int ProcObjectParent(VfsObject* object,VfsObject** out,bool=false)
{
    auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    *out=nullptr;if(int error=ProcObjectSnapshot(value,&snapshot))return error;
    *out=value->parent?value->parent:object;VfsObjectRetain(*out);return 0;
}
// Return 1 for an unsupported descriptor, 0 for a captured directory/regular
// file or pathname-socket capability. Reporting may request other vnode types.
// The FD lock protects acquisition of the path's own references. No FD pin or
// vnode reference is released here: a concurrent close must never make proc's
// outer VFS callback run final descriptor teardown.
static int ProcAcquireVnodeFdPath(const ProcObject* value,VfsPath* out,char* fallback=nullptr,uint32_t capacity=0,bool anyObject=false)
{
    *out={};auto* live=SchedulerFindProcessReference(value->pid);if(!live)return -2;
    if(live->incarnation!=value->incarnation || live->tgid!=value->tgid ||
       (live->isThread?live->leaderIncarnation:live->incarnation)!=value->group) {ProcessUnref(live);return -2;}
    int error=-2;
    auto* fdLock=ProcessFdLock(live);
    SpinLockAcquire(fdLock);
    if(live->fds && value->index<MAX_FDS) {
        const auto& fd=live->fds[value->index];
        if(fd.type!=FdType::None && !fd.closing) {
            error=1;FdDiagnosticPath(fd,fallback,capacity);
            if((fd.type==FdType::Vnode || fd.type==FdType::Path) && fd.handle) {
                auto* node=static_cast<Vnode*>(fd.handle);
                error=-95;
                if(node->mountOwner && node->backendObject) {
                    *out={node->mountOwner,node->backendObject};VfsPathRetain(*out);error=0;
                }
            }
        }
    }
    SpinLockRelease(fdLock);ProcessUnref(live);
    // VnodeType::File also covers FIFOs. Inspect the retained backend identity
    // after dropping the FD lock; pathname sockets allow O_PATH/stat/bind while
    // ordinary opening still fails ENXIO in the backend. These are filesystem
    // nodes, not transport FDs. Character-device inodes (an O_PATH /dev
    // node, which bubblewrap binds through /proc/self/fd) are followed too;
    // their driver comes from st_rdev at open. Never follow textual symlinks.
    // Normal VFS open then rechecks permissions and creates
    // a fresh description, independently of the source offset/access mode.
    if(!error && !anyObject) {
        VnodeStat st{};
        error=out->object->ops->stat?out->object->ops->stat(out->object,&st):-95;
        const uint32_t type=st.mode&0170000;
        if(!error && type!=0040000 && type!=0100000 && type!=0140000 && type!=0020000)error=-95;
        if(error) {VfsPathRelease(*out);*out={};}
    }
    return error;
}
static int ProcObjectReadlink(VfsObject* object,char* out,uint32_t capacity)
{
    auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if(int error=ProcObjectSnapshot(value,&snapshot))return error;
    if(value->kind==ProcObjectKind::NsLink)return ProcNamespaceReadlink(value->pid,value->incarnation,value->tgid,value->group,
        static_cast<ProcNamespaceKind>(value->index),out,capacity);
    PathBuffer target;
    if(!target.allocate())return -12;
    if(value->kind==ProcObjectKind::Self || value->kind==ProcObjectKind::ThreadSelf) {
        Process* current=ProcessCurrent();if(!current)return -2;
        const uint32_t tgid = ProcessVisibleTgid(current, value->pidView);
        if (!tgid) return -2;
        char* end=U64ToDec(target,tgid);
        if(value->kind==ProcObjectKind::ThreadSelf) {
            end=AppendStr(end,"/task/");end=U64ToDec(end,ProcessVisibleTid(current,value->pidView));
        }
        *end=0;
    } else if(value->kind==ProcObjectKind::FdLink) {
        VfsPath path{};int error=ProcAcquireVnodeFdPath(value,&path,target,target.capacity(),true);
        if(!error)error=VfsPathObjectPathFromCallback(path,target,target.capacity());
        VfsPathRelease(path);if(error<0)return error;

    } else if(value->kind==ProcObjectKind::PidFile && value->index==PROC_PID_EXE) {
        const int size=ProcReadExecutableName(snapshot,target,target.capacity());
        if(size<0)return size;
    } else if(value->kind==ProcObjectKind::PidFile && value->index==PROC_PID_CWD) {
        Process* live=SchedulerFindProcessReference(value->pid);if(!live)return -2;
        int error=live->incarnation==value->incarnation?ProcessFsGetcwdFromVfsCallback(live,target,target.capacity()):-2;
        ProcessUnref(live);if(error)return error;
    } else if(value->kind==ProcObjectKind::PidFile && value->index==PROC_PID_ROOT) {
        FsSnapshot fs{};int error=ProcAcquireFsTarget(value->pid,value->incarnation,value->tgid,value->group,&fs);
        if(!error)error=VfsPathDirectoryPathFromCallback(fs.root,target,target.capacity());
        FsSnapshotRelease(&fs);if(error)return error;
    } else return -22;
    uint32_t bytes=ProcStrLen(target);if(bytes>capacity)bytes=capacity;
    memcpy(out,target,bytes);return bytes;
}
static Vnode* ProcObjectOpen(VfsObject* object,int flags,int* error)
{
    *error=0;auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if(value->kind!=ProcObjectKind::NsLink) {
        *error=ProcObjectSnapshot(value,&snapshot);
        if(*error && (*error!=-2 || value->kind!=ProcObjectKind::FdInfoDir))return nullptr;
        if(*error) {
            snapshot={};
            snapshot.pid=value->pid;snapshot.incarnation=value->incarnation;
            snapshot.tgid=value->tgid;snapshot.groupIncarnation=value->group;
            *error=0;
        }
    }
    if(flags&~(VFS_O_METADATA|VFS_O_EXEC|(value->kind==ProcObjectKind::UserMap?(VFS_O_WRITE|VFS_O_READWRITE):0))) { *error=-13;return nullptr; }
    if(flags&VFS_O_EXEC) { *error=-13;return nullptr; }
    if(value->kind==ProcObjectKind::UserMap && !(flags&VFS_O_METADATA)) {
        uint8_t requested=flags&VFS_O_READWRITE?6:flags&VFS_O_WRITE?2:4;
        if((*error=ProcObjectAccess(object,requested,false)))return nullptr;
        if(value->index==3 && (requested&2) && !ProcessCapable(ProcessCurrent(),value->userNamespace,CapSysAdmin)) {
            *error=-13;return nullptr;
        }
    }
    Vnode* node=nullptr;
    VnodeStat metadata{};
    if(flags&VFS_O_METADATA) { *error=ProcObjectStat(object,&metadata);if(*error)return nullptr; }
    if((flags&VFS_O_METADATA) && metadata.isSymlink)node=MakeProcVnode(nullptr,0,false,&g_procMetadataOps);
    else if(value->kind==ProcObjectKind::NsDirectory)node=MakeProcNsDirectory(snapshot);
    else if(value->kind==ProcObjectKind::UserMap)node=MakeUserMapVnode(value->userNamespace,uint8_t(value->index));
    else if(value->kind==ProcObjectKind::Root)node=MakeProcVnode(nullptr,0,true,&g_procRootDirOps);
    else if(value->kind==ProcObjectKind::Net) { *error=-95;return nullptr; }
    else if(value->kind==ProcObjectKind::Sys || value->kind==ProcObjectKind::SysKernel)node=MakeProcSysDirectory(value->kind==ProcObjectKind::SysKernel);
    else if(value->kind==ProcObjectKind::OverflowId)node=GenOverflowId();
    else if(value->kind==ProcObjectKind::FdInfoFile) {
        FsSnapshot actor{};
        *error=ProcAcquireFsTarget(value->pid,value->incarnation,value->tgid,value->group,&actor);
        FsSnapshotRelease(&actor);
        if(*error)return nullptr;
        FdSnapshot fd{};
        if(!SchedulerGetFdInfo(value->pid,value->index,&fd,value->incarnation)) { *error=-2;return nullptr; }
        node=GenPidFdInfo(fd);
    }
    else if(ProcObjectDirectory(value->kind)) {
        ProcLookup path{snapshot,"",value->kind==ProcObjectKind::Task,value->taskMember};
        node=MakePidDirectory(path,value->kind==ProcObjectKind::Task?&g_procTaskDirOps:
                                  value->kind==ProcObjectKind::Fds?&g_procFdDirOps:
                                  value->kind==ProcObjectKind::FdInfoDir?&g_procFdInfoDirOps:&g_procPidDirOps);
    } else if(value->kind==ProcObjectKind::GlobalFile)node=g_globalEntries[value->index].gen();
    else if(value->kind==ProcObjectKind::NetFile) {
        Vnode* (*const generators[])()={GenNetDev,GenNetTcp,GenNetUdp,GenNetSockstat};
        node=generators[value->index]();
    } else if(value->kind==ProcObjectKind::PidFile)node=g_pidEntries[value->index].gen(snapshot);
    else { *error=-40;return nullptr; }
    if(!node) { *error=-12;return nullptr; }
    if(value->kind==ProcObjectKind::PidFile) {
        auto* opened=static_cast<ProcPriv*>(node->priv);
        opened->reportedOwner=(uint64_t{snapshot.euid}<<32)|snapshot.egid;
    }
    VfsObjectRetain(object);node->backendObject=object;return node;
}
static int ProcObjectDirectoryPath(VfsObject* object,char* out,uint32_t capacity)
{
    auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if(int error=ProcObjectSnapshot(value,&snapshot))return error;
    char path[128];char* end=path;*end++='/';
    if(value->kind==ProcObjectKind::Root) {}
    else if(value->kind==ProcObjectKind::Net)end=AppendStr(end,"net");
    else if(value->kind==ProcObjectKind::Sys)end=AppendStr(end,"sys");
    else if(value->kind==ProcObjectKind::SysKernel)end=AppendStr(end,"sys/kernel");
    else {
        if(!ProcObjectDirectory(value->kind))return -20;
        if(value->taskMember) {end=U64ToDec(end,snapshot.viewTid ? snapshot.viewTgid : value->tgid);end=AppendStr(end,"/task/");}
        end=U64ToDec(end,value->viewTid ? value->viewTid : value->pid);
        if(value->kind==ProcObjectKind::Task)end=AppendStr(end,"/task");
        if(value->kind==ProcObjectKind::Fds)end=AppendStr(end,"/fd");
        if(value->kind==ProcObjectKind::FdInfoDir)end=AppendStr(end,"/fdinfo");
        if(value->kind==ProcObjectKind::NsDirectory)end=AppendStr(end,"/ns");
    }
    *end=0;uint32_t bytes=end-path+1;if(bytes>capacity)return -36;
    memcpy(out,path,bytes);return 0;
}
// A followed socket FD is an inode capability, not a pathname to reopen.
// Keep immutable metadata after close/reuse without retaining the transport;
// identities are never recycled. Ordinary opening remains ENXIO like sockfs.
struct ProcSocketTarget { VfsObject object; SocketIdentity identity; };
static void ProcSocketDestroy(VfsObject* object) { kfree(object->priv); }
static int ProcSocketStat(VfsObject* object, VnodeStat* out) {
    auto* target = static_cast<ProcSocketTarget*>(object->priv);
    *out = {}; out->ino = target->identity.inode; out->uid = target->identity.uid;
    out->gid = target->identity.gid; out->mode = 0140666; out->nlink = 1; out->dev = 0;
    return 0;
}
static int ProcSocketAccess(VfsObject* object, uint8_t required, bool realIds) {
    auto* target = static_cast<ProcSocketTarget*>(object->priv);
    return ProcessCheckFileAccess(ProcessCurrent(), target->identity.uid, target->identity.gid,
                                  0140666, required, realIds);
}
static Vnode* ProcSocketOpen(VfsObject* object, int flags, int* error) {
    *error = flags == VFS_O_METADATA ? 0 : -6;
    if (*error) return nullptr;
    auto* node = MakeProcVnode(nullptr, 0, false, &g_procMetadataOps);
    if (!node) { *error = -12; return nullptr; }
    node->backendObject = object; VfsObjectRetain(object); return node;
}
static const VfsObjectOps g_procSocketOps = {
    .destroy=ProcSocketDestroy, .stat=ProcSocketStat, .access=ProcSocketAccess,
    .lookup=nullptr, .parent=nullptr, .readlink=nullptr, .open=ProcSocketOpen,
};
static int ProcSocketPath(const VfsPath& source, const SocketIdentity& identity, VfsPath* out) {
    auto* target = static_cast<ProcSocketTarget*>(kmalloc(sizeof(ProcSocketTarget)));
    if (!target) return -12;
    *target = {}; target->identity = identity;
    target->object = {&g_procSocketOps, target, identity.inode, 1};
    *out = {source.attachment, &target->object};
    VfsPathRetain(*out); VfsObjectRelease(&target->object); return 1;
}

// An anonymous descriptor's pointer-free metadata is valid for this walk
// only. Never create a persistent O_PATH/open handle from a frozen snapshot.
struct ProcAnonStatTarget { VfsObject object; VnodeStat metadata; };
static void ProcAnonStatDestroy(VfsObject* object) { kfree(object->priv); }
static int ProcAnonStat(VfsObject* object, VnodeStat* out) {
    if (!out) return -22;
    *out = static_cast<ProcAnonStatTarget*>(object->priv)->metadata;
    return 0;
}
static int ProcAnonAccess(VfsObject* object, uint8_t required, bool realIds) {
    auto& m = static_cast<ProcAnonStatTarget*>(object->priv)->metadata;
    return ProcessCheckFileAccess(ProcessCurrent(), m.uid, m.gid, m.mode, required, realIds);
}
static Vnode* ProcAnonOpen(VfsObject*, int, int* error) { *error = -95; return nullptr; }
static const VfsObjectOps g_procAnonStatOps = {
    .destroy=ProcAnonStatDestroy, .stat=ProcAnonStat, .access=ProcAnonAccess,
    .lookup=nullptr, .parent=nullptr, .readlink=nullptr, .open=ProcAnonOpen,
};
static int ProcAnonStatPath(const VfsPath& source, const VnodeStat& metadata, VfsPath* out) {
    auto* target = static_cast<ProcAnonStatTarget*>(kmalloc(sizeof(ProcAnonStatTarget)));
    if (!target) return -12;
    static uint64_t nextIdentity = 1;
    const uint64_t identity = __atomic_fetch_add(&nextIdentity, 1, __ATOMIC_RELAXED);
    if (!identity || identity == ~uint64_t{0}) __builtin_trap();
    *target = {}; target->metadata = metadata;
    target->object = {&g_procAnonStatOps, target, identity, 1};
    *out = {source.attachment, &target->object};
    VfsPathRetain(*out); VfsObjectRelease(&target->object); return 1;
}

static int ProcAcquireSocketMetadata(const ProcObject* value, SocketIdentity* out) {
    auto* live = SchedulerFindProcessReference(value->pid);
    if (!live) return -2;
    int error = -2;
    if (live->incarnation == value->incarnation && live->tgid == value->tgid &&
        (live->isThread ? live->leaderIncarnation : live->incarnation) == value->group) {
        error = ProcessReadSocketMetadata(ProcessCurrent(), live, value->index, out);
    }
    ProcessUnref(live); return error;
}

static int ProcObjectFollow(const VfsPath& source,VfsPath* out)
{
    auto* object=source.object;
    *out={};auto* value=static_cast<ProcObject*>(object->priv);
    if(value->kind==ProcObjectKind::NsLink) {
        VfsObject* target=nullptr;
        if(int error=ProcNamespaceObject(value->pid,value->incarnation,value->tgid,value->group,
            static_cast<ProcNamespaceKind>(value->index),&target))return error;
        *out={source.attachment,target};VfsPathRetain(*out);VfsObjectRelease(target);return 1;
    }
    if(value->kind==ProcObjectKind::FdLink) {
        SocketIdentity identity{};
        int socket = ProcAcquireSocketMetadata(value, &identity);
        if (socket < 0) return socket;
        if (socket == 1) return ProcSocketPath(source, identity, out);
        // Match retained root/cwd access policy. This is an exact capability
        // jump, never a reopen of its reported pathname or a mount follow-down.
        FsSnapshot fs{};int error=ProcAcquireFsTarget(value->pid,value->incarnation,value->tgid,value->group,&fs);
        FsSnapshotRelease(&fs);if(error)return error;
        auto* live=SchedulerFindProcessReference(value->pid);
        if(!live)return -2;
        VnodeStat metadata{};
        int anon=-2;
        if(live->incarnation==value->incarnation && live->tgid==value->tgid &&
           (live->isThread?live->leaderIncarnation:live->incarnation)==value->group)
            anon=ProcessReadAnonymousFdMetadata(ProcessCurrent(),live,value->index,&metadata);
        ProcessUnref(live);
        if(anon<0)return anon;
        if(anon==1)return ProcAnonStatPath(source,metadata,out);
        error=ProcAcquireVnodeFdPath(value,out);
        return error==1?-95:error?error:1;
    }
    // Executable identity following remains unsupported.
    if(value->kind==ProcObjectKind::PidFile && value->index==PROC_PID_EXE)return -95;
    if(value->kind!=ProcObjectKind::PidFile ||
       (value->index!=PROC_PID_CWD && value->index!=PROC_PID_ROOT))return 0;
    FsSnapshot fs{};int error=ProcAcquireFsTarget(value->pid,value->incarnation,value->tgid,value->group,&fs);
    if(!error) {
        VfsPath& selected=value->index==PROC_PID_ROOT?fs.root:fs.cwd;
        *out=selected;selected={};
    }
    FsSnapshotRelease(&fs);return error?error:1;
}
static const VfsObjectOps g_procObjectOps={
    .destroy=ProcObjectDestroy,.stat=ProcObjectStat,.access=ProcObjectAccess,
    .lookup=ProcObjectLookup,.parent=ProcObjectParent,.readlink=ProcObjectReadlink,.open=ProcObjectOpen,
    .directory_path=ProcObjectDirectoryPath,.follow_link=ProcObjectFollow,
};
static const VfsObjectOps* ProcObjectOperations() {return &g_procObjectOps;}
static const PidNamespace* ProcOpenedPidView(const Vnode* vnode)
{
    const auto* object = vnode->backendObject;
    return object && object->ops == &g_procObjectOps ? static_cast<const ProcObject*>(object->priv)->pidView : nullptr;
}
int ProcFsRootObject(void* mountPriv,uint8_t,VfsObject** out)
{
    const auto* view = static_cast<const ProcMountView*>(mountPriv);
    return ProcObjectMake(ProcObjectKind::Root,nullptr,nullptr,0,false,out,view ? view->pid : nullptr);
}
int ProcFsVnodeObject(Vnode* node,VfsObject** out)
{
    *out=nullptr;
    if(!node || !node->backendObject || (node->backendObject->ops!=&g_procObjectOps && node->backendObject->ops!=&g_procNamespaceOps))return -95;
    *out=node->backendObject;VfsObjectRetain(*out);return 0;
}

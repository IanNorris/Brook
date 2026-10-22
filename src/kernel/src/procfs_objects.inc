// Retained procfs directory identities. A PID number alone is never authority:
// every per-task operation checks the captured incarnation. No Process strong
// reference is kept (cwd=/proc/self must not create a Process ownership cycle).
enum class ProcObjectKind : uint8_t { Root,Net,Pid,Task,Fds,GlobalFile,NetFile,PidFile,FdLink,Self,ThreadSelf };
struct ProcObject {
    VfsObject object;
    VfsObject* parent;
    ProcObject* next;
    ProcObjectKind kind;
    uint16_t pid,tgid;
    uint64_t incarnation,group;
    bool taskMember;
    uint32_t index;
};
static SpinLock g_procObjectLock;
static ProcObject* g_procObjects;
static uint64_t g_procObjectIdentity=2;
static const VfsObjectOps* ProcObjectOperations();
static bool ProcObjectDirectory(ProcObjectKind kind)
{
    return kind<=ProcObjectKind::Fds;
}
static int ProcObjectSnapshot(const ProcObject* object,ProcessSnapshot* snapshot)
{
    if(!object->pid) { *snapshot={};return 0; }
    if(!SchedulerSnapshotProcess(object->pid,snapshot) || snapshot->incarnation!=object->incarnation ||
       snapshot->tgid!=object->tgid || snapshot->groupIncarnation!=object->group ||
       (object->taskMember && snapshot->state==ProcessState::Terminated))return -2;
    return 0;
}
static int ProcObjectMake(ProcObjectKind kind,VfsObject* parent,const ProcessSnapshot* snapshot,
                           uint32_t index,bool taskMember,VfsObject** out)
{
    *out=nullptr;auto* value=static_cast<ProcObject*>(kmalloc(sizeof(ProcObject)));
    if(!value)return -12;
    *value={};value->kind=kind;value->parent=parent;VfsObjectRetain(parent);
    value->index=index;value->taskMember=taskMember;
    if(snapshot) {
        value->pid=snapshot->pid;value->tgid=snapshot->tgid;
        value->incarnation=snapshot->incarnation;value->group=snapshot->groupIncarnation;
    }
    SpinLockAcquire(&g_procObjectLock);
    for(auto* entry=g_procObjects;entry;entry=entry->next) {
        if(entry->kind==kind && entry->pid==value->pid && entry->incarnation==value->incarnation &&
           entry->tgid==value->tgid && entry->group==value->group && entry->index==index &&
           entry->taskMember==taskMember &&
           ((!entry->parent && !parent) || (entry->parent && parent && entry->parent->ops==parent->ops && entry->parent->identity==parent->identity)) &&
           VfsObjectTryRetain(&entry->object)) {
            *out=&entry->object;SpinLockRelease(&g_procObjectLock);
            VfsObjectRelease(value->parent);kfree(value);return 0;
        }
    }
    // Only root has a fixed identity across zero-reference intervals. Other
    // live keys are interned, using non-reused checked IDs rather than a lossy
    // hash of PID/incarnation/file index.
    if(g_procObjectIdentity==~uint64_t{0})__builtin_trap();
    uint64_t identity=kind==ProcObjectKind::Root?1:g_procObjectIdentity++;
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
    kfree(value);VfsObjectRelease(parent);
}
static int ProcObjectStat(VfsObject* object,VnodeStat* out)
{
    if(!out)return -22;
    auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if(int error=ProcObjectSnapshot(value,&snapshot))return error;
    if(value->kind==ProcObjectKind::FdLink) {
        FdSnapshot fd{};if(!SchedulerGetFdInfo(value->pid,value->index,&fd,value->incarnation))return -2;
    }
    *out={};out->isDir=ProcObjectDirectory(value->kind);
    out->isSymlink=value->kind==ProcObjectKind::FdLink || value->kind==ProcObjectKind::Self ||
                   value->kind==ProcObjectKind::ThreadSelf ||
                   (value->kind==ProcObjectKind::PidFile && (value->index==5 || value->index==6));
    out->mode=out->isDir?0040555:out->isSymlink?0120777:0100444;
    out->ino=object->identity;out->dev=0x70726f63;out->nlink=out->isDir?2:1;
    return 0;
}
static int ProcObjectAccess(VfsObject* object,uint8_t required,bool)
{
    VnodeStat stat{};if(int error=ProcObjectStat(object,&stat))return error;
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
        if(StrEqProc(name,"net"))return ProcObjectMake(ProcObjectKind::Net,object,nullptr,0,false,out);
        for(uint32_t i=0;i<NUM_GLOBAL;++i)
            if(StrEqProc(name,g_globalEntries[i].name))return ProcObjectMake(ProcObjectKind::GlobalFile,object,nullptr,i,false,out);
        const char* end=nullptr;uint32_t pid=ParseUint(name,&end);
        if(!IsDigit(name[0]) || !end || *end || !pid || pid>65535 || !FindProcess(pid,&snapshot))return -2;
        return ProcObjectMake(ProcObjectKind::Pid,object,&snapshot,0,false,out);
    }
    if(value->kind==ProcObjectKind::Net) {
        static const char* names[]={"dev","tcp","udp","sockstat"};
        for(uint32_t i=0;i<4;++i)if(StrEqProc(name,names[i]))return ProcObjectMake(ProcObjectKind::NetFile,object,nullptr,i,false,out);
        return -2;
    }
    if(value->kind==ProcObjectKind::Pid) {
        if(StrEqProc(name,"fd"))return ProcObjectMake(ProcObjectKind::Fds,object,&snapshot,0,value->taskMember,out);
        if(!value->taskMember && StrEqProc(name,"task"))return ProcObjectMake(ProcObjectKind::Task,object,&snapshot,0,false,out);
        for(uint32_t i=0;i<NUM_PID_ENTRIES;++i)
            if(StrEqProc(name,g_pidEntries[i].name))return ProcObjectMake(ProcObjectKind::PidFile,object,&snapshot,i,value->taskMember,out);
        return -2;
    }
    const char* end=nullptr;uint32_t number=ParseUint(name,&end);
    if(!IsDigit(name[0]) || !end || *end)return -2;
    if(value->kind==ProcObjectKind::Task) {
        if(!number || number>65535 || !SchedulerSnapshotTask(value->tgid,value->group,number,0,&snapshot))return -2;
        return ProcObjectMake(ProcObjectKind::Pid,object,&snapshot,0,true,out);
    }
    if(value->kind==ProcObjectKind::Fds) {
        FdSnapshot fd{};if(number>=MAX_FDS || !SchedulerGetFdInfo(value->pid,number,&fd,value->incarnation))return -2;
        return ProcObjectMake(ProcObjectKind::FdLink,object,&snapshot,number,value->taskMember,out);
    }
    return -20;
}
static int ProcObjectParent(VfsObject* object,VfsObject** out,bool=false)
{
    auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    *out=nullptr;if(int error=ProcObjectSnapshot(value,&snapshot))return error;
    *out=value->parent?value->parent:object;VfsObjectRetain(*out);return 0;
}
static int ProcObjectReadlink(VfsObject* object,char* out,uint32_t capacity)
{
    auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if(int error=ProcObjectSnapshot(value,&snapshot))return error;
    char target[512]{};
    if(value->kind==ProcObjectKind::Self || value->kind==ProcObjectKind::ThreadSelf) {
        Process* current=ProcessCurrent();if(!current)return -2;
        char* end=U64ToDec(target,current->tgid?current->tgid:current->pid);
        if(value->kind==ProcObjectKind::ThreadSelf) {end=AppendStr(end,"/task/");end=U64ToDec(end,current->pid);}
        *end=0;
    } else if(value->kind==ProcObjectKind::FdLink) {
        FdSnapshot fd{};if(!SchedulerGetFdInfo(value->pid,value->index,&fd,value->incarnation))return -2;
        ProcStrCopy(target,fd.path,sizeof(target));
    } else if(value->kind==ProcObjectKind::PidFile && value->index==5) {
        ProcStrCopy(target,snapshot.exePath,sizeof(target));
    } else if(value->kind==ProcObjectKind::PidFile && value->index==6) {
        Process* live=SchedulerFindProcessReference(value->pid);if(!live)return -2;
        int error=live->incarnation==value->incarnation?ProcessFsGetcwdFromVfsCallback(live,target,sizeof(target)):-2;
        ProcessUnref(live);if(error)return error;
    } else return -22;
    uint32_t bytes=ProcStrLen(target);if(bytes>capacity)bytes=capacity;
    memcpy(out,target,bytes);return bytes;
}
static Vnode* ProcObjectOpen(VfsObject* object,int flags,int* error)
{
    *error=0;auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if((*error=ProcObjectSnapshot(value,&snapshot)))return nullptr;
    if(flags&~(VFS_O_METADATA|VFS_O_EXEC)) { *error=-13;return nullptr; }
    if(flags&VFS_O_EXEC) { *error=-13;return nullptr; }
    Vnode* node=nullptr;
    if(value->kind==ProcObjectKind::Root)node=MakeProcVnode(nullptr,0,true,&g_procRootDirOps);
    else if(value->kind==ProcObjectKind::Net) { *error=-95;return nullptr; }
    else if(ProcObjectDirectory(value->kind)) {
        ProcLookup path{snapshot,"",value->kind==ProcObjectKind::Task,value->taskMember};
        node=MakePidDirectory(path,value->kind==ProcObjectKind::Task?&g_procTaskDirOps:
                                  value->kind==ProcObjectKind::Fds?&g_procFdDirOps:&g_procPidDirOps);
    } else if(value->kind==ProcObjectKind::GlobalFile)node=g_globalEntries[value->index].gen();
    else if(value->kind==ProcObjectKind::NetFile) {
        Vnode* (*const generators[])()={GenNetDev,GenNetTcp,GenNetUdp,GenNetSockstat};
        node=generators[value->index]();
    } else if(value->kind==ProcObjectKind::PidFile)node=g_pidEntries[value->index].gen(snapshot);
    else { *error=-40;return nullptr; }
    if(!node) { *error=-12;return nullptr; }
    VfsObjectRetain(object);node->backendObject=object;return node;
}
static int ProcObjectDirectoryPath(VfsObject* object,char* out,uint32_t capacity)
{
    auto* value=static_cast<ProcObject*>(object->priv);ProcessSnapshot snapshot{};
    if(int error=ProcObjectSnapshot(value,&snapshot))return error;
    char path[128];char* end=path;*end++='/';
    if(value->kind==ProcObjectKind::Root) {}
    else if(value->kind==ProcObjectKind::Net)end=AppendStr(end,"net");
    else {
        if(!ProcObjectDirectory(value->kind))return -20;
        if(value->taskMember) {end=U64ToDec(end,value->tgid);end=AppendStr(end,"/task/");}
        end=U64ToDec(end,value->pid);
        if(value->kind==ProcObjectKind::Task)end=AppendStr(end,"/task");
        if(value->kind==ProcObjectKind::Fds)end=AppendStr(end,"/fd");
    }
    *end=0;uint32_t bytes=end-path+1;if(bytes>capacity)return -36;
    memcpy(out,path,bytes);return 0;
}
static int ProcObjectFollow(VfsObject* object,VfsPath* out)
{
    *out={};auto* value=static_cast<ProcObject*>(object->priv);
    // Report names for these links, but never pretend reopening the reported
    // spelling preserves an executable/FD identity. Existing explicit proc FD
    // brokers remain separate; general magic FD/ELF following is unsupported.
    if(value->kind==ProcObjectKind::FdLink ||
       (value->kind==ProcObjectKind::PidFile && value->index==5))return -95;
    if(value->kind!=ProcObjectKind::PidFile || value->index!=6)return 0;
    // A ptrace-style cross-process inspection policy is outside this slice.
    // Export exact cwd targets only inside the current process/thread group;
    // checking the target directory alone would bypass private ancestors.
    Process* self=ProcessCurrent();ProcessSnapshot caller{};
    if(!self || !SchedulerSnapshotProcess(self->pid,&caller) || caller.tgid!=value->tgid ||
       caller.groupIncarnation!=value->group)return -13;
    Process* live=SchedulerFindProcessReference(value->pid);if(!live)return -2;
    FsSnapshot fs{};
    int error=live->incarnation==value->incarnation && live->tgid==value->tgid &&
              (live->isThread?live->leaderIncarnation:live->incarnation)==value->group?ProcessFsSnapshot(live,&fs):-2;
    ProcessUnref(live);
    if(!error) {*out=fs.cwd;fs.cwd={};}
    FsSnapshotRelease(&fs);return error?error:1;
}
static const VfsObjectOps g_procObjectOps={
    .destroy=ProcObjectDestroy,.stat=ProcObjectStat,.access=ProcObjectAccess,
    .lookup=ProcObjectLookup,.parent=ProcObjectParent,.readlink=ProcObjectReadlink,.open=ProcObjectOpen,
    .directory_path=ProcObjectDirectoryPath,.follow_link=ProcObjectFollow,
};
static const VfsObjectOps* ProcObjectOperations() {return &g_procObjectOps;}
int ProcFsRootObject(void*,uint8_t,VfsObject** out)
{
    return ProcObjectMake(ProcObjectKind::Root,nullptr,nullptr,0,false,out);
}
int ProcFsVnodeObject(Vnode* node,VfsObject** out)
{
    *out=nullptr;
    if(!node || !node->backendObject || node->backendObject->ops!=&g_procObjectOps)return -95;
    *out=node->backendObject;VfsObjectRetain(*out);return 0;
}

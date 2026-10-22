// Included after g_credentialsLock. No allocation/free or VFS work under it.
static UserAuthority UserAuthorityLocked(Process* process) {
    return {process->userNamespace,process->euid,process->egid,process->capabilities.effective};
}
UserNamespace* ProcessUserNamespaceAcquire(Process* process) {
    if(!process)return nullptr;
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    auto* ns=process->userNamespace;UserNamespaceRetain(ns);
    SpinLockRelease(&g_credentialsLock);return ns;
}
UserAuthority ProcessUserAuthorityAcquire(Process* process) {
    if(!process)return {nullptr,UserIdInvalid,UserIdInvalid,0};
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    auto actor=UserAuthorityLocked(process);UserNamespaceRetain(actor.ns);
    SpinLockRelease(&g_credentialsLock);return actor;
}
bool ProcessCapable(Process* process,UserNamespace* target,unsigned capability) {
    if(!process)return false;
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    bool result=UserNamespaceCapable(UserAuthorityLocked(process),target,capability);
    SpinLockRelease(&g_credentialsLock);return result;
}
uint32_t ProcessReportId(Process* viewer,bool group,uint32_t id) {
    if(!viewer)return id;
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    auto result=UserIdReport(viewer->userNamespace,group,id);
    SpinLockRelease(&g_credentialsLock);return result;
}
uint32_t ProcessResolveId(Process* actor,bool group,uint32_t id) {
    if(!actor)return id;
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    auto result=UserIdToCanonical(actor->userNamespace,group,id);
    SpinLockRelease(&g_credentialsLock);return result;
}
int ProcessUserNamespaceCreate(Process* child) {
    if(!child)return -22;
    auto* ns=static_cast<UserNamespace*>(kmalloc(sizeof(UserNamespace)));if(!ns)return -12;
    *ns={};
    // The child is unpublished. Its inherited identity is a pinned snapshot.
    SpinLockAcquire(&g_credentialsLock);auto* parent=child->userNamespace;
    if((parent && parent->depth>=8) || UserIdFromCanonical(parent,false,child->euid)==UserIdInvalid ||
       UserIdFromCanonical(parent,true,child->egid)==UserIdInvalid) {
        SpinLockRelease(&g_credentialsLock);kfree(ns);return -1;
    }
    static uint64_t nextIdentity=1;
    if(nextIdentity==~uint64_t{0})__builtin_trap();
    ns->identity=nextIdentity++;ns->parent=parent;UserNamespaceRetain(parent);
    ns->owner=child->euid;ns->creatorGroup=child->egid;ns->depth=parent?parent->depth+1:1;
    ns->creatorSetfcap=UserNamespaceCapable(UserAuthorityLocked(child),parent,CapSetfcap);
    ns->denyGroups=parent && parent->denyGroups;
    child->userNamespace=ns;child->capabilities={UserCapabilities,UserCapabilities,0,UserCapabilities};
    SpinLockRelease(&g_credentialsLock);UserNamespaceRelease(parent);return 0;
}
int ProcessReadCapabilities(Process* process,UserCapabilityState* out) {
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    if(!process->userNamespace) {SpinLockRelease(&g_credentialsLock);return -38;}
    *out=process->capabilities;SpinLockRelease(&g_credentialsLock);return 0;
}
int ProcessSetCapabilities(Process* process,uint64_t effective,uint64_t permitted,uint64_t inheritable) {
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    int result=process->userNamespace?UserCapabilitiesSet(process->capabilities,effective,permitted,inheritable):-38;
    SpinLockRelease(&g_credentialsLock);return result;
}
int ProcessCapabilityControl(Process* process,unsigned option,uint64_t argument) {
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    int result=-22;
    if(process->userNamespace && argument<=40 && (option==23 || option==24)) {
        if(option==23)result=(process->capabilities.bounding>>argument)&1;
        else if(!UserNamespaceCapable(UserAuthorityLocked(process),process->userNamespace,CapSetpcap))result=-1;
        else {process->capabilities.bounding&=~(uint64_t{1}<<argument);result=0;}
    }
    SpinLockRelease(&g_credentialsLock);return result;
}
bool ProcessSetgroupsAllowed(Process* process) {
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    auto* ns=process->userNamespace;
    bool result=UserNamespaceCapable(UserAuthorityLocked(process),ns,CapSetgid) &&
        (!ns || (ns->groups.written && !ns->denyGroups));
    SpinLockRelease(&g_credentialsLock);return result;
}
static bool UserMapNumber(const char*& cursor,const char* end,uint32_t* out) {
    while(cursor<end && (*cursor==' ' || *cursor=='\t' || *cursor=='\n'))++cursor;
    if(cursor==end || *cursor<'0' || *cursor>'9')return false;
    uint64_t value=0;
    do {value=value*10+unsigned(*cursor++-'0');if(value>0xffffffffULL)return false;}
    while(cursor<end && *cursor>='0' && *cursor<='9');
    if(cursor<end && *cursor!=' ' && *cursor!='\t' && *cursor!='\n')return false;
    *out=uint32_t(value);return true;
}
int ProcessUserMapWrite(UserNamespace* ns,const UserAuthority& opened,const char* text,uint64_t length,bool group,bool setgroups) {
    if(!ns || !length || length>255)return -22;
    uint32_t inner=0,outer=0,count=0;bool deny=false;
    if(setgroups) {
        if(length && text[length-1]=='\n')--length;
        if(length==4 && memcmp(text,"deny",4)==0)deny=true;
        else if(length!=5 || memcmp(text,"allow",5)!=0)return -22;
    } else {
        const char* cursor=text;const char* end=text+length;
        if(!UserMapNumber(cursor,end,&inner) || !UserMapNumber(cursor,end,&outer) || !UserMapNumber(cursor,end,&count))return -22;
        while(cursor<end && (*cursor==' ' || *cursor=='\t' || *cursor=='\n'))++cursor;
        if(cursor!=end)return -22;
    }
    auto* process=ProcessCurrent();if(!process)return -1;
    ProcessOperationGuard operation(process);SpinLockAcquire(&g_credentialsLock);
    auto actor=UserAuthorityLocked(process);int result=-1;
    // Linux's single-ID mapping path delegates the opening credential's
    // authority with the open description (also across SCM_RIGHTS). Requiring
    // the receiver's current user namespace here would break that contract.
    if(UserNamespaceCapable(opened,ns,CapSysAdmin) &&
       (!group || setgroups || ns->denyGroups || UserNamespaceCapable(actor,ns->parent,CapSetgid)))
        result=setgroups?UserNamespaceSetgroups(ns,opened,deny):UserNamespaceWriteMap(ns,opened,group,inner,outer,count);
    SpinLockRelease(&g_credentialsLock);return result;
}
uint32_t ProcessUserMapRead(UserNamespace* ns,UserNamespace* viewer,bool group,bool setgroups,char* out,uint32_t capacity) {
    auto* process=ProcessCurrent();uint32_t inner=0,outer=0,count=0;bool written=false,deny=false;
    ProcessOperationGuard operation(process);SpinLockAcquire(&g_credentialsLock);
    if(ns) {
        deny=ns->denyGroups;const auto& map=group?ns->groups:ns->users;written=map.written;
        inner=map.inner;count=1;
        outer=UserIdFromCanonical(viewer==ns?ns->parent:viewer,group,map.canonical);
    } else {written=true;count=0xffffffffu;outer=UserIdFromCanonical(viewer,group,0);}
    SpinLockRelease(&g_credentialsLock);
    if(setgroups) {
        const char* value=deny?"deny\n":"allow\n";uint32_t n=deny?5:6;
        if(capacity<n)return 0;memcpy(out,value,n);return n;
    }
    if(!written)return 0;
    char buffer[64];uint32_t size=0;
    const uint32_t values[]={inner,outer,count};
    for(auto value:values) {
        char digits[10];unsigned n=0;do {digits[n++]=char('0'+value%10);value/=10;}while(value);
        while(n)buffer[size++]=digits[--n];buffer[size++]=' ';
    }
    buffer[size-1]='\n';if(capacity<size)return 0;memcpy(out,buffer,size);return size;
}

bool ProcessCanSignal(Process* sender,Process* target) {
    if(!sender || !target)return false;
    ProcessOperationGuard operation(ProcessCurrent());SpinLockAcquire(&g_credentialsLock);
    bool result=sender->uid==target->uid || sender->uid==target->suid ||
        sender->euid==target->uid || sender->euid==target->suid ||
        UserNamespaceCapable(UserAuthorityLocked(sender),target->userNamespace,5);
    SpinLockRelease(&g_credentialsLock);return result;
}
void ProcessSignalInfoToUser(Process* viewer,SignalInfo* info) {
    if(info && (info->si_code==0 || info->si_code==-6 || info->si_code==-1 ||
       (info->si_signo==17 && info->si_code>=1 && info->si_code<=6))) {
        uint32_t id;memcpy(&id,info->_data+4,4);id=ProcessReportId(viewer,false,id);memcpy(info->_data+4,&id,4);
    }
}

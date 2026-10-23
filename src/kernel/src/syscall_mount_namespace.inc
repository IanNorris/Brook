// Bounded mount slice: directory binds, private-only propagation and detach.
// No shared/master topology exists; reject types that would create one.
static int64_t sys_mount(uint64_t sourceAddr,uint64_t targetAddr,uint64_t fsAddr,
                         uint64_t flags,uint64_t dataAddr,uint64_t)
{
    constexpr uint64_t Bind=0x1000,Remount=0x20,Recursive=0x4000,Silent=0x8000;
    // Linux accepts the historical magic prefix only when all its bits match.
    if((flags&0xffff0000ULL)==0xc0ed0000ULL)flags&=~0xffff0000ULL;
    constexpr uint64_t Private=0x40000,Slave=0x80000;
    const uint64_t type=flags&~(Recursive|Silent);
    const bool privatePropagation=type==Private || type==Slave;
    const bool memoryMount=(flags&~Silent)==6; // bounded NODEV|NOSUID tmpfs
    const bool bindMount=type==Bind;
    const bool remount=(flags&~(Silent|uint64_t{7}))==(Bind|Remount) && (flags&2)==2;
    if(!bindMount && !privatePropagation && !memoryMount && !remount)return -EINVAL;
    TmpfsMountOptions memoryOptions{};
    if(memoryMount) {
        char fsType[32],data[256];
        if(!CopyUserCString(fsAddr,fsType,sizeof(fsType)))return -EFAULT;
        if(!StrEq(fsType,"tmpfs"))return -ENODEV;
        if(dataAddr) {
            if(!CopyUserCString(dataAddr,data,sizeof(data)))return -EFAULT;
            if(int error=ParseTmpfsMountOptions(data,&memoryOptions))return error;
        }
    }
    auto* process=ProcessCurrent();if(!process)return -EPERM;
    char source[512],target[512];
    if(!CopyUserCString(targetAddr,target,sizeof(target)))return -EFAULT;
    // Propagation has no source operand. A non-null source is still a userspace
    // string argument to mount(2), so validate it without resolving its path.
    if((sourceAddr || (!privatePropagation && !memoryMount && !remount)) && !CopyUserCString(sourceAddr,source,sizeof(source)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,target))return error;
    auto* owner=VfsMountNamespaceUserOwnerAcquire(lookup.fs.mountNamespace);
    bool authorized=ProcessCapable(process,owner,CapSysAdmin);UserNamespaceRelease(owner);
    if(!authorized)return -EPERM;
    if(remount) {
        VfsPath targetPath{};int error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,target,&targetPath);
        if(!error)error=VfsMountNamespaceRemount(lookup.fs.mountNamespace,targetPath,flags&7);
        VfsPathRelease(targetPath);return error;
    }
    if(memoryMount) {
        VfsPath targetPath{};int error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,target,&targetPath);
        if(!error) {
            auto* fsOwner=ProcessUserNamespaceAcquire(process);
            auto* instance=TmpfsCreate(ProcessReadIds(process,false).effective,
                ProcessReadIds(process,true).effective,fsOwner,6,&error,memoryOptions.mode);
            UserNamespaceRelease(fsOwner);
            if(instance) {
                error=VfsMountNamespaceMountFresh(lookup.fs.mountNamespace,targetPath,"tmpfs",TmpfsOperations(),instance,0,6);
                if(error)TmpfsDestroy(instance);
            }
        }
        VfsPathRelease(targetPath);return error;
    }
    if(privatePropagation) {
        VfsPath targetPath{};
        int error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,target,&targetPath);
        if(!error)error=VfsMountNamespacePrivatePropagation(lookup.fs.mountNamespace,targetPath);
        VfsPathRelease(targetPath);return error;
    }
    if(!source[0])return -ENOENT;
    return VfsMountNamespaceBindAt(lookup.fs.mountNamespace,lookup.fs.root,lookup.fs.cwd,source,target,(flags&Recursive)!=0);
}
static int64_t sys_umount2(uint64_t targetAddr,uint64_t flags,uint64_t,
                           uint64_t,uint64_t,uint64_t)
{
    if(flags!=2)return -EINVAL;
    auto* process=ProcessCurrent();if(!process)return -EPERM;
    char target[512];if(!CopyUserCString(targetAddr,target,sizeof(target)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,target))return error;
    auto* owner=VfsMountNamespaceUserOwnerAcquire(lookup.fs.mountNamespace);
    bool authorized=ProcessCapable(process,owner,CapSysAdmin);UserNamespaceRelease(owner);
    if(!authorized)return -EPERM;
    VfsPath path{};int error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,target,&path);
    if(!error)error=VfsMountNamespaceDetach(lookup.fs.mountNamespace,path);
    VfsPathRelease(path);return error;
}

static int64_t sys_pivot_root(uint64_t newRootAddr,uint64_t putOldAddr,uint64_t,uint64_t,uint64_t,uint64_t)
{
    auto* process=ProcessCurrent();if(!process)return -EPERM;
    char newName[512],oldName[512];
    if(!CopyUserCString(newRootAddr,newName,sizeof(newName)) || !CopyUserCString(putOldAddr,oldName,sizeof(oldName)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,newName))return error;
    auto* owner=VfsMountNamespaceUserOwnerAcquire(lookup.fs.mountNamespace);
    bool authorized=ProcessCapable(process,owner,CapSysAdmin);UserNamespaceRelease(owner);
    if(!authorized)return -EPERM;
    VfsPath newRoot{},putOld{};int error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,newName,&newRoot);
    if(!error)error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,oldName,&putOld);
    if(!error)error=VfsMountNamespacePivotRoot(lookup.fs.mountNamespace,lookup.fs.root,newRoot,putOld);
    VfsPathRelease(putOld);VfsPathRelease(newRoot);return error;
}

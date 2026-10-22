// Bounded mount slice: directory binds, private-only propagation and detach.
// No shared/master topology exists; reject types that would create one.
static int64_t sys_mount(uint64_t sourceAddr,uint64_t targetAddr,uint64_t fsAddr,
                         uint64_t flags,uint64_t dataAddr,uint64_t)
{
    constexpr uint64_t Bind=0x1000,Recursive=0x4000,Silent=0x8000;
    constexpr uint64_t Private=0x40000,Slave=0x80000;
    const uint64_t type=flags&~(Recursive|Silent);
    const bool privatePropagation=type==Private || type==Slave;
    const bool memoryMount=(flags&~Silent)==6; // bounded NODEV|NOSUID tmpfs
    if(flags!=Bind && !privatePropagation && !memoryMount)return -EINVAL;
    if(memoryMount) {
        char fsType[32],data[256];
        if(!CopyUserCString(fsAddr,fsType,sizeof(fsType)))return -EFAULT;
        if(!StrEq(fsType,"tmpfs"))return -ENODEV;
        if(dataAddr) {
            if(!CopyUserCString(dataAddr,data,sizeof(data)))return -EFAULT;
            if(data[0])return -EINVAL;
        }
    }
    auto* process=ProcessCurrent();if(!process)return -EPERM;
    char source[512],target[512];
    if(!CopyUserCString(targetAddr,target,sizeof(target)))return -EFAULT;
    // Propagation has no source operand. A non-null source is still a userspace
    // string argument to mount(2), so validate it without resolving its path.
    if((sourceAddr || (!privatePropagation && !memoryMount)) && !CopyUserCString(sourceAddr,source,sizeof(source)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,target))return error;
    auto* owner=VfsMountNamespaceUserOwnerAcquire(lookup.fs.mountNamespace);
    bool authorized=ProcessCapable(process,owner,CapSysAdmin);UserNamespaceRelease(owner);
    if(!authorized)return -EPERM;
    if(memoryMount) {
        VfsPath targetPath{};int error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,target,&targetPath);
        if(!error) {
            auto* fsOwner=ProcessUserNamespaceAcquire(process);
            auto* instance=TmpfsCreate(ProcessReadIds(process,false).effective,
                ProcessReadIds(process,true).effective,fsOwner,6,&error);
            UserNamespaceRelease(fsOwner);
            if(instance) {
                error=VfsMountNamespaceMountFresh(lookup.fs.mountNamespace,targetPath,"tmpfs",TmpfsOperations(),instance);
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
    VfsPath sourcePath{},targetPath{};
    int error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,source,&sourcePath);
    if(!error)error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,target,&targetPath);
    if(!error)error=VfsMountNamespaceBind(lookup.fs.mountNamespace,sourcePath,targetPath);
    VfsPathRelease(targetPath);VfsPathRelease(sourcePath);return error;
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

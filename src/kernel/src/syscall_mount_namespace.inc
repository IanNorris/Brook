// Minimum privileged mount slice: directory MS_BIND and subtree MNT_DETACH.
// Linux ignores filesystem/data for MS_BIND; other mount classes/flags are
// rejected before any path or topology mutation, never acknowledged as no-ops.
static int64_t sys_mount(uint64_t sourceAddr,uint64_t targetAddr,uint64_t,
                         uint64_t flags,uint64_t,uint64_t)
{
    if(flags!=0x1000)return -EINVAL;
    auto* process=ProcessCurrent();if(!process || process->euid!=0)return -EPERM;
    char source[512],target[512];
    if(!CopyUserCString(sourceAddr,source,sizeof(source)) || !CopyUserCString(targetAddr,target,sizeof(target)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,target))return error;
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
    auto* process=ProcessCurrent();if(!process || process->euid!=0)return -EPERM;
    char target[512];if(!CopyUserCString(targetAddr,target,sizeof(target)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,target))return error;
    VfsPath path{};int error=VfsPathResolve(lookup.fs.root,lookup.fs.cwd,target,&path);
    if(!error)error=VfsMountNamespaceDetach(lookup.fs.mountNamespace,path);
    VfsPathRelease(path);return error;
}

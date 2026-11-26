#include "syscall_user_path.h"
// syscall_path.cpp -- path namespace syscalls: cwd, mkdir, unlink, rename, link, symlink, readlink, umask.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process_user_namespace.h"

#include "user_access_scope.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_memfd.h"
#include "syscall_open.h"
#include "syscall_handlers.h"
#include "nv_drm.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_getcwd (79)
// ---------------------------------------------------------------------------

int64_t sys_getcwd(uint64_t bufAddr, uint64_t size, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    PathBuffer cwd; if (!cwd.allocate()) return -ENOMEM;int error=ProcessFsGetcwd(ProcessCurrent(),cwd,cwd.capacity());
    if(error)return error;
    size_t bytes=strlen(cwd)+1;
    if(size<bytes)return -ERANGE;
    return CopyToUser(bufAddr,cwd,bytes)?static_cast<int64_t>(bytes):-EFAULT;
}

// ---------------------------------------------------------------------------
// sys_readlink (89) / sys_readlinkat (267)
// ---------------------------------------------------------------------------

static int64_t DoReadlink(const char* original,uint64_t bufAddr,uint64_t bufsiz,int dirfd=-100)
{
    AtLookup at;if(int error=at.Init(dirfd,original))return error;
    char synthetic[512];
    const char* path=!at.SyntheticName(original,synthetic,sizeof(synthetic))?synthetic:"";
    if (bufsiz == 0) return -EINVAL;

    // GL shim (M0): the DRM device's 'subsystem' symlink. libdrm's
    // get_subsystem_type() readlinks this and matches the basename to classify
    // the bus; returning ".../pci" yields DRM_BUS_PCI for the virtio-gpu node.
    if ((StrEq(path, "/sys/dev/char/226:128/device/subsystem") && brook::GpuAppGet()) ||
        (StrEq(path, "/sys/dev/char/226:129/device/subsystem") && brook::BdwIntelUserNodeEnabled()) ||
        (StrEq(path, "/sys/dev/char/226:130/device/subsystem") && brook::NvDrmNodeEnabled()))
    {
        const char* tgt = "/sys/bus/pci";
        uint64_t slen = 0; while (tgt[slen]) ++slen;
        if (slen > bufsiz) slen = bufsiz;
        if (!CopyToUser(bufAddr, tgt, slen)) return -EFAULT;
        return static_cast<int64_t>(slen);
    }

    // Proc symlinks resolve through the attached procfs instance. A global
    // numeric lookup here would bypass the mount's PID view.
    // Try VFS readlink for real symlinks
    PathBuffer linkTarget; if (!linkTarget.allocate()) return -ENOMEM;
    uint64_t cap = (bufsiz < linkTarget.capacity()) ? bufsiz : linkTarget.capacity();
    int r=VfsReadlinkAt(at.fs.root,at.start(),original,linkTarget,static_cast<uint32_t>(cap));
    if (r >= 0 && !CopyToUser(bufAddr, linkTarget, static_cast<uint64_t>(r)))
        return -EFAULT;
    if (r >= 0) return static_cast<int64_t>(r);
    return r;
}

int64_t sys_readlink(uint64_t pathAddr, uint64_t bufAddr, uint64_t bufsiz,
                      uint64_t, uint64_t, uint64_t)
{
    UserAccessScope access(ProcessCurrent());
    PathBuffer path;
    if (int error = CopyUserPath(pathAddr, path)) return error;
    return DoReadlink(path,bufAddr,bufsiz);
}

int64_t sys_readlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t bufAddr,
                        uint64_t bufsiz, uint64_t, uint64_t)
{
    UserAccessScope access(ProcessCurrent());
    PathBuffer path;
    if (int error = CopyUserPath(pathAddr, path)) return error;

    if(!path[0]) {
        if(!bufsiz)return -EINVAL;
        if(static_cast<int>(dirfd)==-100)return -ENOENT;
        FdRef ref(ProcessCurrent(),static_cast<int>(dirfd));if(!ref)return -EBADF;
        if((ref->type!=FdType::Path && ref->type!=FdType::Vnode) || !ref->handle)return -EINVAL;
        PathBuffer target; if (!target.allocate()) return -ENOMEM;
        VfsPath retained{};int error=VfsPathFromVnode(static_cast<Vnode*>(ref->handle),&retained);
        uint32_t capacity=bufsiz<target.capacity()?static_cast<uint32_t>(bufsiz):target.capacity();
        if(!error)error=VfsPathReadlink(retained,target,capacity);
        VfsPathRelease(retained);
        if(error>=0 && !CopyToUser(bufAddr,target,static_cast<uint64_t>(error)))return -EFAULT;
        return error;
    }
    return DoReadlink(path,bufAddr,bufsiz,static_cast<int>(dirfd));
}

// ---------------------------------------------------------------------------
// sys_symlink (88) / sys_symlinkat (266)
// ---------------------------------------------------------------------------

int64_t sys_symlink(uint64_t targetAddr, uint64_t linkpathAddr, uint64_t,
                     uint64_t, uint64_t, uint64_t)
{
    PathBuffer target, path;
    if (int error = CopyUserPath(targetAddr, target)) return error;
    if (int error = CopyUserPath(linkpathAddr, path)) return error;
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    return VfsSymlinkAt(lookup.fs.root,lookup.start(),target,path);
}

int64_t sys_symlinkat(uint64_t targetAddr, uint64_t newdirfd, uint64_t linkpathAddr,
                       uint64_t, uint64_t, uint64_t)
{
    PathBuffer target, path;
    if (int error = CopyUserPath(targetAddr, target)) return error;
    if (int error = CopyUserPath(linkpathAddr, path)) return error;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(newdirfd),path))return error;
    return VfsSymlinkAt(lookup.fs.root,lookup.start(),target,path);
}

// ---------------------------------------------------------------------------
// sys_umask (95) — set process umask, return old value
// ---------------------------------------------------------------------------

int64_t sys_umask(uint64_t newMask, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return ProcessFsExchangeMask(proc,static_cast<uint16_t>(newMask));
}

// ---------------------------------------------------------------------------
// sys_chdir (80) / sys_fchdir (81) — canonical path and final search check
// ---------------------------------------------------------------------------

int64_t sys_chdir(uint64_t pathAddr,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    PathBuffer path;if (int error = CopyUserPath(pathAddr, path)) return error;
    for(unsigned attempt=0;attempt<8;++attempt) {
        AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
        VfsPath cwd{};int error=VfsPathResolve(lookup.fs.root,lookup.start(),path,&cwd);
        if(!error)error=ProcessFsSetResolvedCwd(ProcessCurrent(),cwd,lookup.fs);
        VfsPathRelease(cwd);if(error!=-EAGAIN)return error;
    }
    return -EAGAIN;
}

int64_t sys_chroot(uint64_t pathAddr,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    auto* process=ProcessCurrent();
    if(!process)return -EPERM;
    auto* owner=ProcessUserNamespaceAcquire(process);
    const bool authorized=ProcessCapable(process,owner,CapSysChroot);
    UserNamespaceRelease(owner);
    if(!authorized)return -EPERM;

    PathBuffer path;if (int error = CopyUserPath(pathAddr, path)) return error;
    for(unsigned attempt=0;attempt<8;++attempt) {
        AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
        VfsPath root{};int error=VfsPathResolve(lookup.fs.root,lookup.start(),path,&root);
        auto* context=ProcessFsAcquire(process);
        if(!error)error=FsContextSetResolvedRoot(context,root,lookup.fs);
        FsContextRelease(context);VfsPathRelease(root);
        if(error!=-EAGAIN)return error;
    }
    return -EAGAIN;
}

int64_t sys_fchdir(uint64_t fd,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    Process* proc=ProcessCurrent();if(!proc)return -EBADF;
    VfsPath cwd{};int error=0;
    { FdRef ref(proc,static_cast<int>(fd));
      if(!ref)return -EBADF;
      if((ref->type!=FdType::Vnode && ref->type!=FdType::Path) || !ref->handle)return -ENOTDIR;
      error=VfsPathFromVnode(static_cast<Vnode*>(ref->handle),&cwd); }
    if(!error)error=ProcessFsSetCwd(proc,cwd);
    VfsPathRelease(cwd);return error;
}

// ---------------------------------------------------------------------------
// sys_rmdir (84) — remove an empty directory
// ---------------------------------------------------------------------------

static int64_t DoUnlink(const char* path, bool directory = false);

int64_t sys_rmdir(uint64_t pathAddr, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    PathBuffer pathBuf;
    if (int error = CopyUserPath(pathAddr, pathBuf)) return error;
    return DoUnlink(pathBuf, true);
}

// ---------------------------------------------------------------------------
// sys_unlink (87) — delete a file
// ---------------------------------------------------------------------------

static int64_t DoUnlink(const char* path, bool directory)
{
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    char synthetic[512];
    if(!directory && !lookup.SyntheticName(path,synthetic,sizeof(synthetic)) &&
       !strncmp(synthetic,"/dev/shm/",9))return NamedShmUnlink(synthetic+9);
    return VfsRemoveAt(lookup.fs.root,lookup.start(),path,directory);
}

int64_t sys_unlink(uint64_t pathAddr, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    PathBuffer pathBuf;
    if (int error = CopyUserPath(pathAddr, pathBuf)) return error;
    return DoUnlink(pathBuf);
}

// ---------------------------------------------------------------------------
// sys_rename (82) — rename a file or directory
// ---------------------------------------------------------------------------

static int64_t DoRename(const char* oldBuf, const char* newBuf)
{
    AtLookup oldLookup,newLookup;
    if(int error=oldLookup.Init(-100,oldBuf))return error;
    if(int error=newLookup.Init(-100,newBuf))return error;
    return VfsRenameAt(oldLookup.fs.root,oldLookup.start(),oldBuf,newLookup.start(),newBuf);
}

int64_t sys_rename(uint64_t oldAddr, uint64_t newAddr, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    PathBuffer oldBuf, newBuf;
    if (int error = CopyUserPath(oldAddr, oldBuf)) return error;
    if (int error = CopyUserPath(newAddr, newBuf)) return error;
    return DoRename(oldBuf, newBuf);
}

static int64_t DoMkdir(const char* path, uint16_t mode)
{
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    return VfsMkdirAt(lookup.fs.root,lookup.start(),path,mode,lookup.fs.mask);
}

int64_t sys_mkdir(uint64_t pathAddr, uint64_t mode, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    PathBuffer pathBuf;
    if (int error = CopyUserPath(pathAddr, pathBuf)) return error;
    return DoMkdir(pathBuf,static_cast<uint16_t>(mode));
}

// ---------------------------------------------------------------------------
// sys_mkdirat (258) — create a directory relative to dirfd
// ---------------------------------------------------------------------------

int64_t sys_mknodat(uint64_t dirfd,uint64_t pathAddr,uint64_t mode,uint64_t device,uint64_t,uint64_t)
{
    if((mode&0170000)!=0010000)return -95;
    if(device)return -EINVAL;
    PathBuffer path;if (int error = CopyUserPath(pathAddr, path)) return error;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    int error=0;Vnode* node=VfsCreateFifoAt(lookup.fs.root,lookup.start(),path,static_cast<uint16_t>(mode),lookup.fs.mask,&error);
    if(!node)return error?error:-EIO;
    VfsClose(node);return 0;
}
int64_t sys_mknod(uint64_t path,uint64_t mode,uint64_t device,uint64_t,uint64_t,uint64_t)
{ return sys_mknodat(static_cast<uint64_t>(-100),path,mode,device,0,0); }

int64_t sys_mkdirat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                     uint64_t, uint64_t, uint64_t)
{
    PathBuffer path;if (int error = CopyUserPath(pathAddr, path)) return error;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    return VfsMkdirAt(lookup.fs.root,lookup.start(),path,static_cast<uint16_t>(mode),lookup.fs.mask);
}

// ---------------------------------------------------------------------------
// sys_unlinkat (263) — unlink file or rmdir relative to dirfd
// ---------------------------------------------------------------------------

int64_t sys_unlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                      uint64_t, uint64_t, uint64_t)
{
    if(flags & ~0x200ULL)return -EINVAL;
    PathBuffer path;if (int error = CopyUserPath(pathAddr, path)) return error;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    char synthetic[512];
    if(!(flags&0x200) && !lookup.SyntheticName(path,synthetic,sizeof(synthetic)) &&
       !strncmp(synthetic,"/dev/shm/",9))return NamedShmUnlink(synthetic+9);
    return VfsRemoveAt(lookup.fs.root,lookup.start(),path,(flags&0x200)!=0);
}

// ---------------------------------------------------------------------------
// sys_renameat / sys_renameat2 (264 / 316)
// ---------------------------------------------------------------------------

int64_t sys_renameat(uint64_t olddirfd, uint64_t oldAddr,
                      uint64_t newdirfd, uint64_t newAddr,
                      uint64_t, uint64_t)
{
    PathBuffer oldP, newP;
    if (int error = CopyUserPath(oldAddr, oldP)) return error;
    if (int error = CopyUserPath(newAddr, newP)) return error;
    AtLookup oldLookup,newLookup;
    if(int error=oldLookup.Init(static_cast<int>(olddirfd),oldP))return error;
    if(int error=newLookup.Init(static_cast<int>(newdirfd),newP))return error;
    return VfsRenameAt(oldLookup.fs.root,oldLookup.start(),oldP,newLookup.start(),newP);
}

int64_t sys_renameat2(uint64_t olddirfd, uint64_t oldAddr,
                       uint64_t newdirfd, uint64_t newAddr,
                       uint64_t flags, uint64_t)
{
    // RENAME_NOREPLACE is checked under the same retained mutation lock as
    // publication. Other atomic rename policies remain unsupported.
    if (flags & ~uint64_t{1}) return -EINVAL;
    if(!flags)return sys_renameat(olddirfd, oldAddr, newdirfd, newAddr, 0, 0);
    PathBuffer oldP, newP;
    if (int error = CopyUserPath(oldAddr, oldP)) return error;
    if (int error = CopyUserPath(newAddr, newP)) return error;
    AtLookup oldLookup,newLookup;
    if(int error=oldLookup.Init(static_cast<int>(olddirfd),oldP))return error;
    if(int error=newLookup.Init(static_cast<int>(newdirfd),newP))return error;
    return VfsRenameNoReplaceAt(oldLookup.fs.root,oldLookup.start(),oldP,newLookup.start(),newP);
}

// ---------------------------------------------------------------------------
// sys_linkat (265) — create hard link
// ---------------------------------------------------------------------------

int64_t sys_linkat(uint64_t olddirfd, uint64_t oldAddr,
                    uint64_t newdirfd, uint64_t newAddr,
                    uint64_t flags, uint64_t)
{
    constexpr uint64_t FOLLOW=0x400,EMPTY_PATH=0x1000;
    if(flags&~(FOLLOW|EMPTY_PATH))return -EINVAL;
    PathBuffer oldPath, newPath;
    if (int error = CopyUserPath(oldAddr, oldPath)) return error;
    if (int error = CopyUserPath(newAddr, newPath)) return error;
    AtLookup destination;if(int error=destination.Init(static_cast<int>(newdirfd),newPath))return error;
    if(!oldPath[0]) {
        if(!(flags&EMPTY_PATH))return -ENOENT;
        // Conservative subset: open-time credential snapshots are not yet
        // retained, so only capability-authorized empty-source links are admitted.
        if(!ProcessCapable(ProcessCurrent(),nullptr,CapDacReadSearch))return -ENOENT;
        FdRef fd(ProcessCurrent(),static_cast<int>(olddirfd));if(!fd)return -EBADF;
        if((fd->type!=FdType::Path && fd->type!=FdType::Vnode) || !fd->handle)return -95;
        VfsPath source{};int error=VfsPathFromVnode(static_cast<Vnode*>(fd->handle),&source);
        if(!error)error=VfsLinkPathAt(destination.fs.root,source,destination.start(),newPath);
        VfsPathRelease(source);return error;
    }
    AtLookup source;if(int error=source.Init(static_cast<int>(olddirfd),oldPath))return error;
    return VfsLinkAt(source.fs.root,source.start(),oldPath,destination.start(),newPath,flags&FOLLOW);
}

// ---------------------------------------------------------------------------
// sys_link (86) — create hard link
// ---------------------------------------------------------------------------

int64_t sys_link(uint64_t oldAddr, uint64_t newAddr, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    return sys_linkat(static_cast<uint64_t>(-100),oldAddr,static_cast<uint64_t>(-100),newAddr,0,0);
}

} // namespace brook

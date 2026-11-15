// syscall_dir.cpp -- directory listing: getdents64.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "intel_bdw_scanout.h"
#include "syscall_internal.h"
#include "user_copy.h"
#include "syscall_drm.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_getdents64 (217) -- directory listing
// ---------------------------------------------------------------------------

struct LinuxDirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

// One record is built in kernel memory and copied out with the checked user
// copy. Names are at most 255 bytes, so a record is at most 280 bytes.
static constexpr uint64_t DirentHeader = 19;
static constexpr uint64_t DirentMax = (DirentHeader + 255 + 1 + 7) & ~7ULL;
static bool CopyDirent(uint64_t address, uint64_t ino, int64_t off, uint8_t type,
                       const char* name, uint64_t nameLen, uint64_t reclen)
{
    alignas(8) uint8_t record[DirentMax] = {};
    auto* ent = reinterpret_cast<LinuxDirent64*>(record);
    ent->d_ino = ino;
    ent->d_off = off;
    ent->d_reclen = static_cast<uint16_t>(reclen);
    ent->d_type = type;
    for (uint64_t i = 0; i < nameLen; ++i) ent->d_name[i] = name[i];
    return CopyToUser(address, record, reclen);
}

int64_t sys_getdents64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                        uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef pinned(proc,static_cast<int>(fd));
    if(!pinned)return -EBADF;
    FdEntry* fde=pinned.get();

    // Synthetic /dev/dri directory (GL shim M0): list ".", "..", "renderD128"
    // so libdrm's drmGetDevices2 can discover the render node.
    if (fde->type == FdType::SyntheticMem && fde->handle == g_drmDriDirTag)
    {
        const char* driEntries[4]={".","..",nullptr,nullptr};uint32_t entries=2;
        if(brook::GpuAppGet())driEntries[entries++]="renderD128";
        if(brook::BdwIntelUserNodeEnabled())driEntries[entries++]="renderD129";
        uint64_t opos = 0;
        FdPosition position(fde);
        uint32_t ck = static_cast<uint32_t>(position.value());
        while (ck < entries)
        {
            const char* nm = driEntries[ck];
            uint64_t nameLen = 0;
            while (nm[nameLen]) ++nameLen;
            uint64_t reclen = (19 + nameLen + 1 + 7) & ~7ULL;
            if (opos + reclen > count) break;
            // DT_DIR for . / .. ; DT_CHR for the node
            if (!CopyDirent(bufAddr + opos, ck + 1, static_cast<int64_t>(ck + 1),
                            (ck < 2) ? 4 : 2, nm, nameLen, reclen)) {
                if (opos == 0) return -EFAULT;
                break;
            }
            opos += reclen;
            ++ck;
        }
        position.set(ck);
        return static_cast<int64_t>(opos);
    }

    if (fde->type != FdType::Vnode || !fde->handle) return -EBADF;
    auto* vn = static_cast<Vnode*>(fde->handle);
    uint64_t pos = 0;
    // The directory cookie is the shared description offset (H-20), held
    // across the whole batch as Linux holds f_pos_lock for getdents64.
    FdPosition position(fde);
    uint32_t cookie = static_cast<uint32_t>(position.value());

    DirEntry de; de.name[0] = 0; de.size = 0; de.isDir = false;
    while (pos < count) {
        uint32_t nextCookie = cookie;
        int ret=VfsReaddir(vn,&de,&nextCookie);
        if(ret<0) {if(!pos)return ret;break;}
        if(!ret)break;

        uint64_t nameLen = 0;
        while (de.name[nameLen] && nameLen < 255) ++nameLen;

        // d_name starts at offset 19 in LinuxDirent64
        uint64_t reclen = (19 + nameLen + 1 + 7) & ~7ULL;
        if (pos + reclen > count) {
            if (pos == 0) return -EINVAL;
            break;
        }
        // DirEntry only distinguishes directories. Other entries can be
        // symlinks or special files: DT_UNKNOWN asks callers to use lstat,
        // whereas falsely claiming DT_REG breaks ldconfig's inode cache.
        // The cookie advances only once the record reached the caller.
        if (!CopyDirent(bufAddr + pos, nextCookie + 1, static_cast<int64_t>(nextCookie),
                        de.isDir ? 4 : 0, de.name, nameLen, reclen)) {
            if (pos == 0) return -EFAULT;
            break;
        }
        cookie = nextCookie;

        pos += reclen;
    }

    position.set(cookie);
    return static_cast<int64_t>(pos);
}

} // namespace brook

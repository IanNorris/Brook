// syscall_dir.cpp -- directory listing: getdents64.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "intel_bdw_scanout.h"
#include "syscall_internal.h"
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
        auto* obuf = reinterpret_cast<uint8_t*>(bufAddr);
        uint64_t opos = 0;
        uint32_t ck = static_cast<uint32_t>(fde->seekPos);
        while (ck < entries)
        {
            const char* nm = driEntries[ck];
            uint64_t nameLen = 0;
            while (nm[nameLen]) ++nameLen;
            uint64_t reclen = (19 + nameLen + 1 + 7) & ~7ULL;
            if (opos + reclen > count) break;
            auto* ent = reinterpret_cast<LinuxDirent64*>(obuf + opos);
            ent->d_ino = ck + 1;
            ent->d_off = static_cast<int64_t>(ck + 1);
            ent->d_reclen = static_cast<uint16_t>(reclen);
            ent->d_type = (ck < 2) ? 4 : 2; // DT_DIR for . / .. ; DT_CHR for the node
            for (uint64_t i = 0; i < nameLen; ++i) ent->d_name[i] = nm[i];
            for (uint64_t i = nameLen; i < reclen - 19; ++i) ent->d_name[i] = '\0';
            opos += reclen;
            ++ck;
        }
        fde->seekPos = ck;
        return static_cast<int64_t>(opos);
    }

    if (fde->type != FdType::Vnode || !fde->handle) return -EBADF;
    auto* vn = static_cast<Vnode*>(fde->handle);
    auto* buf = reinterpret_cast<uint8_t*>(bufAddr);
    uint64_t pos = 0;
    uint32_t cookie = static_cast<uint32_t>(fde->seekPos);

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
        cookie = nextCookie;

        auto* ent = reinterpret_cast<LinuxDirent64*>(buf + pos);
        ent->d_ino = cookie + 1;
        ent->d_off = static_cast<int64_t>(cookie);
        ent->d_reclen = static_cast<uint16_t>(reclen);
        // DirEntry only distinguishes directories. Other entries can be
        // symlinks or special files: DT_UNKNOWN asks callers to use lstat,
        // whereas falsely claiming DT_REG breaks ldconfig's inode cache.
        ent->d_type = de.isDir ? 4 : 0; // DT_DIR : DT_UNKNOWN

        for (uint64_t i = 0; i < nameLen; ++i) ent->d_name[i] = de.name[i];
        ent->d_name[nameLen] = '\0';
        for (uint64_t i = nameLen + 1; i < reclen - 19; ++i)
            ent->d_name[i] = '\0';

        pos += reclen;
    }

    fde->seekPos = cookie;
    return static_cast<int64_t>(pos);
}

} // namespace brook

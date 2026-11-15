#pragma once

// syscall_open.h -- internal declarations shared from syscall_open.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "process_fs.h"

#include "intel_bdw_scanout.h"
#include "string.h"
#include "syscall_internal.h"
#include "syscall_drm.h"

namespace brook {

// Own a filesystem snapshot and, when used, an independently retained dirfd
// object. Descriptor pins are dropped after acquisition, never under VFS locks.
struct AtLookup {
    FsSnapshot fs{};
    VfsPath directory{};
    bool drmDirectory=false;
    const VfsPath& start() const { return directory.object?directory:fs.cwd; }
    void Clear() { VfsPathRelease(directory);directory={};FsSnapshotRelease(&fs); }
    ~AtLookup() { Clear(); }
    int Init(int fd,const char* text) {
        if(!text)return -EINVAL;
        if(!text[0])return -ENOENT;
        int error=ProcessFsSnapshot(ProcessCurrent(),&fs);if(error)return error;
        if(text[0]=='/') {directory=fs.root;VfsPathRetain(directory);return 0;}
        if(fd==-100)return 0;
        FdRef ref(ProcessCurrent(),fd);
        if(!ref)return -EBADF;
        if(ref->type==FdType::SyntheticMem && ref->handle==g_drmDriDirTag){
            const bool intel=brook::BdwIntelUserNodeEnabled();
            if(!GpuAppGet() && !intel)return -ENOTDIR;
            bool allowed=false;error=VfsCanonicalDrmBrokerAllowed(fs.root,false,&allowed,!GpuAppGet() && intel);
            if(error)return error;if(!allowed)return -ENOTDIR;
            // Resolve the retained canonical directory in the caller's root;
            // SyntheticName will revalidate its visible identity and broker
            // each child before dispatch, including after mount changes.
            error=VfsPathResolve(fs.root,fs.root,"/dev/dri",&directory);
            if(!error)drmDirectory=true;
            return error;
        }
        if((ref->type!=FdType::Vnode && ref->type!=FdType::Path) || !ref->handle)return -ENOTDIR;
        auto* vnode=static_cast<Vnode*>(ref->handle);
        if(vnode->type!=VnodeType::Dir)return -ENOTDIR;
        return VfsPathFromVnode(vnode,&directory);
    }
    int BrokerAllowed(const char* absolute,bool* allowed) const {
        if(brook::BdwIntelUserNodeEnabled()){
            const bool node=StrEq(absolute,"/dev/dri/renderD129");
            const bool sysfs=IntelDrmSysIsDir(absolute) || IntelDrmSysAttrContent(absolute) ||
                StrEq(absolute,"/sys/dev/char/226:129/device/subsystem");
            if(node || sysfs)return VfsCanonicalDrmBrokerAllowed(fs.root,sysfs,allowed,true);
            if(StrEq(absolute,"/dev/dri") && !GpuAppGet())return VfsCanonicalDrmBrokerAllowed(fs.root,false,allowed,true);
        }
        if(GpuAppGet()) {
            const bool device=StrEq(absolute,"/dev/dri") || StrEq(absolute,"/dev/dri/renderD128");
            const bool sysfs=DrmSysIsDir(absolute) || DrmSysAttrContent(absolute) ||
                StrEq(absolute,"/sys/dev/char/226:128/device/subsystem");
            if(device || sysfs)return VfsCanonicalDrmBrokerAllowed(fs.root,sysfs,allowed);
        }
        return VfsPathBrokerAllowed(fs.root,absolute,allowed);
    }
    // Synthetic proc/dev and historical aliases only. Never feed this name to
    // normal filesystem operations. Reject hidden retained starts: a current
    // global label must not redirect a lower cwd into an overmount.
    int SyntheticName(const char* text,char* out,uint32_t capacity) const {
        if(drmDirectory && (StrEq(text,"renderD128") || StrEq(text,"renderD129"))){
            const char* prefix="/dev/dri/";const uint32_t prefixLen=9,suffixLen=uint32_t(strlen(text));
            if(prefixLen+suffixLen+1>capacity)return -36;
            memcpy(out,prefix,prefixLen);memcpy(out+prefixLen,text,suffixLen+1);
            bool allowed=false;const int error=BrokerAllowed(out,&allowed);
            return error?error:allowed?0:-95;
        }
        if(text[0]=='/') {
            size_t bytes=strlen(text)+1;if(bytes>capacity)return -36;
            memcpy(out,text,bytes);bool allowed=false;
            int error=BrokerAllowed(out,&allowed);return error?error:allowed?0:-95;
        }
        char parent[512];int error=VfsPathDirectoryPath(start(),parent,sizeof(parent));
        if(error)return error;
        VfsPath visible{};error=VfsPathResolve(fs.root,fs.root,parent,&visible);
        bool same=!error && VfsPathSame(start(),visible);VfsPathRelease(visible);
        if(!same)return error?error:-95;
        size_t prefix=strlen(parent),suffix=strlen(text);
        bool slash=prefix && parent[prefix-1]!='/';
        if(prefix+slash+suffix>=capacity)return -36;
        memcpy(out,parent,prefix);if(slash)out[prefix++]='/';
        memcpy(out+prefix,text,suffix+1);bool allowed=false;
        error=BrokerAllowed(out,&allowed);return error?error:allowed?0:-95;
    }
    AtLookup()=default;AtLookup(const AtLookup&)=delete;AtLookup& operator=(const AtLookup&)=delete;
};

} // namespace brook

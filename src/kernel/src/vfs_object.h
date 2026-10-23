#pragma once
#include <stdint.h>

namespace brook {
struct Vnode;
struct VnodeStat;
struct VnodeTime;
struct VfsObject;
struct VfsPath;

// Backend identity, independent of a file description's flags, offset and
// directory-iteration state. Its owner must separately pin the mount backing.
// Each successful root/from-vnode/lookup/parent call returns one owned object.
// Callbacks take borrowed live objects and acquire backend locks themselves;
// the caller holds the outer VFS transaction through lookup and operation.
struct VfsObjectOps {
    void (*destroy)(VfsObject*);
    int (*stat)(VfsObject*,VnodeStat*);
    int (*access)(VfsObject*,uint8_t required,bool realIds);
    // Exactly one component, no symlink following or mount transitions.
    // Match the walk's real/effective-ID selection when rechecking search.
    int (*lookup)(VfsObject* directory,const char* name,VfsObject**,bool realIds);
    int (*parent)(VfsObject* directory,VfsObject**,bool realIds);
    int (*readlink)(VfsObject*,char*,uint32_t capacity);
    // Open this exact object, not its name. Creation is a parent operation;
    // truncation remains at the VFS mapping/alias invalidation gate.
    Vnode* (*open)(VfsObject*,int flags,int* error);
    // Parent operations receive exactly one validated non-dot component.
    // Creation modes are already masked by the caller's FsContext snapshot;
    // callbacks must not acquire process/context locks under backend locks.
    // create exclusively publishes a new regular file and returns its vnode.
    Vnode* (*create)(VfsObject* parent,const char* name,int flags,uint16_t mode,int* error) = nullptr;
    int (*mkdir)(VfsObject* parent,const char* name,uint16_t mode) = nullptr;
    int (*remove)(VfsObject* parent,const char* name,bool directory) = nullptr;
    int (*rename)(VfsObject* oldParent,const char* oldName,VfsObject* newParent,const char* newName) = nullptr;
    int (*symlink)(VfsObject* parent,const char* target,const char* name) = nullptr;
    Vnode* (*create_socket)(VfsObject* parent,const char* name,uint16_t mode,int* error) = nullptr;
    int (*set_times)(VfsObject*,const VnodeTime*) = nullptr;
    int (*chmod)(VfsObject*,uint16_t mode) = nullptr;
    int (*chown)(VfsObject*,uint32_t uid,uint32_t gid) = nullptr;
    // Reporting only; never re-resolve this name for a subsequent operation.
    int (*directory_path)(VfsObject*,char* out,uint32_t capacity) = nullptr;
    // 0: ordinary textual symlink; 1: owned exact magic-link target in out;
    // negative: explicit unsupported/error. Called only for actual following,
    // never for readlink reporting, within the namespace transaction. Source is
    // borrowed and pins its attachment, allowing exact pseudo-object targets.
    int (*follow_link)(const VfsPath& source,VfsPath* out) = nullptr;
    // Reporting only, for an exact retained entry of any type. Unsupported
    // backends must fail rather than substitute a diagnostic/opening label.
    int (*object_path)(VfsObject*,char* out,uint32_t capacity) = nullptr;
    Vnode* (*create_fifo)(VfsObject* parent,const char* name,uint16_t mode,int* error) = nullptr;
};
struct VfsObject {
    const VfsObjectOps* ops;
    void* priv;
    uint64_t identity;
    uint32_t references;
};
inline void VfsObjectRetain(VfsObject* object) {
    if(!object)return;
    uint32_t count=__atomic_load_n(&object->references,__ATOMIC_RELAXED);
    do {
        if(!count || count==~uint32_t{0})__builtin_trap();
    } while(!__atomic_compare_exchange_n(&object->references,&count,count+1,false,
                                         __ATOMIC_RELAXED,__ATOMIC_RELAXED));
}
// Acquire from a registry-protected pointer without resurrecting a final
// release waiting to unlink it. The registry lock must prevent destruction.
inline bool VfsObjectTryRetain(VfsObject* object) {
    if(!object)return false;
    uint32_t count=__atomic_load_n(&object->references,__ATOMIC_ACQUIRE);
    while(count) {
        if(count==~uint32_t{0})__builtin_trap();
        if(__atomic_compare_exchange_n(&object->references,&count,count+1,false,
                                       __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return true;
    }
    return false;
}
// May destroy backend state; release outside process/context spinlocks.
inline void VfsObjectRelease(VfsObject* object) {
    if(!object)return;
    uint32_t count=__atomic_load_n(&object->references,__ATOMIC_RELAXED);
    do {
        if(!count)__builtin_trap();
    } while(!__atomic_compare_exchange_n(&object->references,&count,count-1,false,
                                         __ATOMIC_ACQ_REL,__ATOMIC_RELAXED));
    if(count==1)object->ops->destroy(object);
}
} // namespace brook

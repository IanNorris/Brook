#pragma once
#include <stdint.h>

namespace brook {
struct Vnode;
struct VnodeStat;
struct VfsObject;

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
    int (*lookup)(VfsObject* directory,const char* name,VfsObject**);
    int (*parent)(VfsObject* directory,VfsObject**);
    int (*readlink)(VfsObject*,char*,uint32_t capacity);
    // Open this exact object, not its name. Creation is a parent operation;
    // truncation remains at the VFS mapping/alias invalidation gate.
    Vnode* (*open)(VfsObject*,int flags,int* error);
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

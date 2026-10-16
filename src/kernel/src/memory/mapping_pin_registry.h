#pragma once
#include <stdint.h>
#include "spinlock.h"

namespace brook {
struct Vnode;
// Lifetime-only mapping pins. Callers retain their vnode independently.
// First Pin and namespace/resize checks must share the VFS mutation mutex.
// RetainIfPinned/Unpin cannot create an identity and need no sleeping lock.
// Hash buckets bound lookup work, not capacity: every entry is dynamic.
class MappingPinRegistry {
public:
    MappingPinRegistry() = default;
    MappingPinRegistry(const MappingPinRegistry&) = delete;
    MappingPinRegistry& operator=(const MappingPinRegistry&) = delete;
    bool RetainIfPinned(Vnode* vnode);
    int Pin(Vnode* vnode,uint64_t device,uint64_t inode);
    void Unpin(Vnode* vnode);
    bool Busy(uint64_t device,uint64_t inode,bool wholeDevice = false);
private:
    struct Identity {
        uint64_t device,inode,pins;
        Identity* next;
    };
    struct Binding {
        Vnode* vnode;
        uint64_t pins;
        Identity* identity;
        Binding* next;
    };
    static constexpr uint32_t BindingBuckets=1024,IdentityBuckets=256;
    static uint32_t BindingBucket(Vnode* vnode);
    static uint32_t IdentityBucket(uint64_t device,uint64_t inode);
    Binding* FindBinding(Vnode* vnode);
    Identity* FindIdentity(uint64_t device,uint64_t inode);
    static void Retain(Binding* binding);
    Binding* bindings_[BindingBuckets]{};
    Identity* identities_[IdentityBuckets]{};
    IrqSpinLock lock_;
};
}

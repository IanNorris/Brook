#pragma once
#include <stdint.h>

namespace brook {
// The non-allocating publication/storage state of a mount namespace. The VFS
// topology transaction protects its attachment registry, not this state. A
// caller must own a storage reference throughout every operation below.
//
// Each task owns a member lease independently of FsContext sharing or snapshot
// references. Last departure makes ALL this namespace's edges unsearchable at
// the release CAS. Retired records may remain on a deferred cleanup list, but
// every lookup must check Published(); retaining storage never revives edges.
// No callback, allocation, destructor, scheduler call or lock is taken here.
class MountNamespaceState {
    static constexpr uint32_t Retired = uint32_t{1} << 31;
    uint32_t storageReferences_ = 1;
    uint32_t members_ = 0; // zero is a private, prepared namespace, not published
public:
    MountNamespaceState() = default;
    MountNamespaceState(const MountNamespaceState&) = delete;
    MountNamespaceState& operator=(const MountNamespaceState&) = delete;

    // Only a fully prepared new namespace may activate its first member. An
    // existing namespace must use TryJoin(), which cannot revive retirement.
    bool ActivatePrepared() {
        uint32_t expected=0;
        return __atomic_compare_exchange_n(&members_,&expected,1,false,
                                            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    }
    bool TryJoin() {
        uint32_t value=__atomic_load_n(&members_,__ATOMIC_ACQUIRE);
        for(;;) {
            if(!value || value>=Retired-1)return false;
            if(__atomic_compare_exchange_n(&members_,&value,value+1,false,
                                            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return true;
        }
    }
    // Exactly one caller observes true. It must arrange deferred topology
    // retirement while retaining storage; no backend work belongs in this call.
    bool LeaveMember() {
        uint32_t value=__atomic_load_n(&members_,__ATOMIC_ACQUIRE);
        for(;;) {
            if(!value || (value&Retired))__builtin_trap();
            const uint32_t next=value==1?Retired:value-1;
            if(__atomic_compare_exchange_n(&members_,&value,next,false,
                                            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return next==Retired;
        }
    }
    // Rollback of a never-published clone. Caller still releases its private
    // attachment registry; there is no task/member lease to drop.
    bool AbandonPrepared() {
        uint32_t expected=0;
        return __atomic_compare_exchange_n(&members_,&expected,Retired,false,
                                            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    }
    bool Published() const {
        const uint32_t value=__atomic_load_n(&members_,__ATOMIC_ACQUIRE);
        return value && !(value&Retired);
    }
    bool IsRetired() const {
        return (__atomic_load_n(&members_,__ATOMIC_ACQUIRE)&Retired)!=0;
    }
    void RetainStorage() {
        uint32_t value=__atomic_load_n(&storageReferences_,__ATOMIC_RELAXED);
        for(;;) {
            if(!value || value==~uint32_t{0})__builtin_trap();
            if(__atomic_compare_exchange_n(&storageReferences_,&value,value+1,false,
                                            __ATOMIC_RELAXED,__ATOMIC_RELAXED))return;
        }
    }
    // true transfers final destruction responsibility to the caller. Registry
    // removal, backend release and kfree must happen outside publication locks.
    // Outstanding members must own storage and cannot be silently discarded.
    bool ReleaseStorage() {
        uint32_t value=__atomic_load_n(&storageReferences_,__ATOMIC_ACQUIRE);
        for(;;) {
            if(!value || (value==1 && Published()))__builtin_trap();
            if(__atomic_compare_exchange_n(&storageReferences_,&value,value-1,false,
                                            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return value==1;
        }
    }
};
} // namespace brook

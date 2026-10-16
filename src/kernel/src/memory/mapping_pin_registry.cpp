#include "mapping_pin_registry.h"
#include "heap.h"
#include "panic.h"

namespace brook {
namespace {
uint64_t Mix(uint64_t value) {
    value^=value>>30;value*=0xbf58476d1ce4e5b9ULL;
    value^=value>>27;value*=0x94d049bb133111ebULL;
    return value^(value>>31);
}
}
uint32_t MappingPinRegistry::BindingBucket(Vnode* vnode) {
    return Mix(reinterpret_cast<uint64_t>(vnode))&(BindingBuckets-1);
}
uint32_t MappingPinRegistry::IdentityBucket(uint64_t device,uint64_t inode) {
    return Mix(device^Mix(inode))&(IdentityBuckets-1);
}
MappingPinRegistry::Binding* MappingPinRegistry::FindBinding(Vnode* vnode) {
    for(auto* binding=bindings_[BindingBucket(vnode)];binding;binding=binding->next)
        if(binding->vnode==vnode)return binding;
    return nullptr;
}
MappingPinRegistry::Identity* MappingPinRegistry::FindIdentity(uint64_t device,uint64_t inode) {
    for(auto* identity=identities_[IdentityBucket(device,inode)];identity;identity=identity->next)
        if(identity->device==device && identity->inode==inode)return identity;
    return nullptr;
}
void MappingPinRegistry::Retain(Binding* binding) {
    if(binding->pins==~uint64_t{0} || binding->identity->pins==~uint64_t{0})
        KernelPanic("VFS: mapping pin count overflow");
    ++binding->pins;++binding->identity->pins;
}
bool MappingPinRegistry::RetainIfPinned(Vnode* vnode) {
    uint64_t flags=IrqSpinLockAcquire(&lock_);
    auto* binding=FindBinding(vnode);
    if(binding)Retain(binding);
    IrqSpinLockRelease(&lock_,flags);
    return binding!=nullptr;
}
int MappingPinRegistry::Pin(Vnode* vnode,uint64_t device,uint64_t inode) {
    if(!vnode || !device || !inode)return -95;
    if(RetainIfPinned(vnode))return 0;
    // Speculative storage is never allocated/freed under the registry lock.
    // The identity candidate is optional if another binding already pins it.
    auto* candidate=static_cast<Binding*>(kmalloc(sizeof(Binding)));
    auto* identityCandidate=candidate?static_cast<Identity*>(kmalloc(sizeof(Identity))):nullptr;
    int result=-12;
    uint64_t flags=IrqSpinLockAcquire(&lock_);
    auto* binding=FindBinding(vnode);
    if(binding) { Retain(binding);result=0; }
    else if(candidate) {
        auto* identity=FindIdentity(device,inode);
        if(identity || identityCandidate) {
            if(!identity) {
                uint32_t bucket=IdentityBucket(device,inode);
                *identityCandidate={device,inode,0,identities_[bucket]};
                identity=identityCandidate;identities_[bucket]=identity;identityCandidate=nullptr;
            }
            uint32_t bucket=BindingBucket(vnode);
            *candidate={vnode,0,identity,bindings_[bucket]};
            Retain(candidate);bindings_[bucket]=candidate;candidate=nullptr;result=0;
        }
    }
    IrqSpinLockRelease(&lock_,flags);
    kfree(candidate);kfree(identityCandidate);
    return result;
}
void MappingPinRegistry::Unpin(Vnode* vnode) {
    Binding* retiredBinding=nullptr;Identity* retiredIdentity=nullptr;
    uint64_t flags=IrqSpinLockAcquire(&lock_);
    auto** link=&bindings_[BindingBucket(vnode)];
    while(*link && (*link)->vnode!=vnode)link=&(*link)->next;
    auto* binding=*link;
    if(!binding || !binding->pins || !binding->identity->pins)
        KernelPanic("VFS: mapping pin underflow");
    auto* identity=binding->identity;
    --binding->pins;--identity->pins;
    if(!binding->pins) { *link=binding->next;retiredBinding=binding; }
    if(!identity->pins) {
        auto** identityLink=&identities_[IdentityBucket(identity->device,identity->inode)];
        while(*identityLink && *identityLink!=identity)identityLink=&(*identityLink)->next;
        if(!*identityLink)KernelPanic("VFS: mapping identity missing");
        *identityLink=identity->next;retiredIdentity=identity;
    }
    IrqSpinLockRelease(&lock_,flags);
    kfree(retiredBinding);kfree(retiredIdentity);
}
bool MappingPinRegistry::Busy(uint64_t device,uint64_t inode,bool wholeDevice) {
    uint64_t flags=IrqSpinLockAcquire(&lock_);bool busy=false;
    if(!wholeDevice)busy=FindIdentity(device,inode)!=nullptr;
    else {
        for(auto* bucket:identities_) {
            for(auto* identity=bucket;identity;identity=identity->next)
                if(identity->device==device) { busy=true;break; }
            if(busy)break;
        }
    }
    IrqSpinLockRelease(&lock_,flags);return busy;
}
}

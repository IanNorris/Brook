#include "file_mapping_registry.h"
#include "file_object.h"
#include "mapping_ranges.h"
#include "address_space.h"
#include "heap.h"
#include "spinlock.h"
#include "panic.h"
#include "string.h"
#ifndef BROOK_HOST_TEST
#include "process.h"
#endif

namespace brook {
namespace {
IrqSpinLock registryLock;
void RequireFileGate(FileObject* file,AddressSpaceState* space) {
#ifndef BROOK_HOST_TEST
    // Ref-zero teardown has no publishers; registry exclusion protects weak
    // acquisition until its nodes are unlinked, without a sleeping file lock.
    if(__atomic_load_n(&space->references,__ATOMIC_ACQUIRE) &&
       (!file->gate.locked || file->gate.owner!=ProcessCurrent()))
        KernelPanic("file mapping: publication without file gate");
#else
    (void)file;(void)space;
#endif
}
}
void FileMappingRegister(FileMappingRegistration* registration,AddressSpaceState* space,MappingBacking* backing) {
    if(!space || !backing || !backing->file)return;
    if(registration->file)KernelPanic("file mapping: registration already linked");
    auto* file=backing->file;RequireFileGate(file,space);
    uint64_t flags=IrqSpinLockAcquire(&registryLock);
    *registration={space,file,file->mappings,space->identity};file->mappings=registration;
    if(__atomic_add_fetch(&file->mappingRegistrations,1,__ATOMIC_RELEASE)==0)
        KernelPanic("file mapping: token count overflow");
    IrqSpinLockRelease(&registryLock,flags);
}
void FileMappingRetire(FileMappingRegistration* registration) {
    if(!registration->file)return;
    RequireFileGate(registration->file,registration->space);
    uint64_t flags=IrqSpinLockAcquire(&registryLock);registration->retiring=true;
    IrqSpinLockRelease(&registryLock,flags);
}
void FileMappingUnregister(FileMappingRegistration* registration) {
    auto* file=registration->file;if(!file)return;
    // Removal publishes no alias. Caller has completed the retirement ACK (or
    // ref-zero teardown). Taking the file gate here would deadlock a truncator
    // awaiting pending retired tokens while holding that same gate.
    uint64_t flags=IrqSpinLockAcquire(&registryLock);
    auto** link=&file->mappings;
    while(*link && *link!=registration)link=&(*link)->next;
    if(!*link)KernelPanic("file mapping: registration missing");
    *link=registration->next;*registration={};
    if(__atomic_fetch_sub(&file->mappingRegistrations,1,__ATOMIC_ACQ_REL)==0)
        KernelPanic("file mapping: token count underflow");
    IrqSpinLockRelease(&registryLock,flags);
}
AddressSpaceState* FileMappingNextSpace(FileObject* file,uint64_t afterIdentity) {
    uint64_t flags=IrqSpinLockAcquire(&registryLock);
    for(;;) {
        FileMappingRegistration* found=nullptr;
        for(auto* token=file->mappings;token;token=token->next)
            if(token->spaceIdentity>afterIdentity && (!found || token->spaceIdentity<found->spaceIdentity) &&
               __atomic_load_n(&token->space->references,__ATOMIC_ACQUIRE))found=token;
        if(!found) { IrqSpinLockRelease(&registryLock,flags);return nullptr; }
        if(AddressSpaceTryRef(found->space)) {
            auto* space=found->space;IrqSpinLockRelease(&registryLock,flags);return space;
        }
        afterIdentity=found->spaceIdentity;
    }
}
uint64_t FileMappingRegisteredRanges(FileObject* file) {
    uint64_t flags=IrqSpinLockAcquire(&registryLock),count=0;
    for(auto* token=file->mappings;token;token=token->next)++count;
    if(count!=__atomic_load_n(&file->mappingRegistrations,__ATOMIC_ACQUIRE))
        KernelPanic("file mapping: token count mismatch");
    IrqSpinLockRelease(&registryLock,flags);return count;
}
uint64_t FileMappingRetiringRanges(FileObject* file) {
    uint64_t flags=IrqSpinLockAcquire(&registryLock),count=0;
    for(auto* token=file->mappings;token;token=token->next)
        if(token->retiring || !__atomic_load_n(&token->space->references,__ATOMIC_ACQUIRE))++count;
    IrqSpinLockRelease(&registryLock,flags);return count;
}
FileMappingGateSet::~FileMappingGateSet() { Finish(); }
void FileMappingGateSet::ReleasePins() {
    for(uint64_t i=0;i<count_;++i)MappingBackingUnref(pins_[i].backing);
    count_=0;
}
bool FileMappingGateSet::Add(MappingBacking* backing) {
    if(!backing || !backing->file)return true;
    uint64_t slot=0;
    if(index_) {
        uint64_t hash=backing->file->orderId;
        hash=(hash^(hash>>30))*0xbf58476d1ce4e5b9ULL;
        hash=(hash^(hash>>27))*0x94d049bb133111ebULL;
        slot=(hash^(hash>>31))&(indexCapacity_-1);
        uint64_t probes=0;
        while(index_[slot]) {
            if(index_[slot]>count_ || ++probes==indexCapacity_)
                KernelPanic("file mapping: invalid snapshot index");
            if(pins_[index_[slot]-1].file==backing->file)return true;
            slot=(slot+1)&(indexCapacity_-1);
        }
    } else {
        // Small spaces retain the allocation-free bounded eight-file path.
        for(uint64_t i=0;i<count_;++i)if(pins_[i].file==backing->file)return true;
    }
    if(count_==capacity_)return false;
    MappingBackingRef(backing);pins_[count_++]={backing,backing->file};
    if(index_)index_[slot]=count_;
    return true;
}
void FileMappingGateSet::SortPins() {
    // Allocation-free heapsort of distinct files, outside AS mutation. The
    // snapshot index is no longer used until the next collection clears it.
    auto swap=[&](uint64_t a,uint64_t b) { Pin pin=pins_[a];pins_[a]=pins_[b];pins_[b]=pin; };
    auto sift=[&](uint64_t root,uint64_t end) {
        while(root<end/2) {
            uint64_t child=root*2+1;
            if(child+1<end && pins_[child].file->orderId<pins_[child+1].file->orderId)++child;
            if(pins_[root].file->orderId>=pins_[child].file->orderId)break;
            swap(root,child);root=child;
        }
    };
    for(uint64_t root=count_/2;root>0;--root)sift(root-1,count_);
    for(uint64_t end=count_;end>1;--end) { swap(0,end-1);sift(0,end-1); }
}
bool FileMappingGateSet::Lock(AddressSpaceState* space,MappingBacking* incoming,MappingBacking* onlyBacking) {
    if(space_ || count_)KernelPanic("file mapping: recursive gate set");
    for(;;) {
        if(index_)memset(index_,0,indexCapacity_*sizeof(uint64_t));
        KMutexLock(&space->mutation);
        uint64_t generation=space->generation,needed=1;bool fits=Add(incoming);
        for(auto* node=space->ranges.head;node;node=node->next) {
            if(onlyBacking && node->range.backing!=onlyBacking)continue;
            if(node->range.backing && node->range.backing->file)++needed;
            if(fits)fits=Add(node->range.backing);
        }
        if(fits && !count_) { space_=space;return true; }
        KMutexUnlock(&space->mutation);
        if(!fits) {
            ReleasePins();
            if(needed>~uint64_t{0}/sizeof(Pin) || needed>~uint64_t{0}/2)return false;
            uint64_t slots=16;
            while(slots<needed*2) {
                if(slots>~uint64_t{0}/2)return false;
                slots*=2;
            }
            const uint64_t pinBytes=needed*sizeof(Pin);
            if(slots>(~uint64_t{0}-pinBytes)/sizeof(uint64_t))return false;
            auto* larger=static_cast<Pin*>(kmalloc(pinBytes+slots*sizeof(uint64_t)));if(!larger)return false;
            if(pins_!=local_)kfree(pins_);
            pins_=larger;capacity_=needed;
            index_=reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(larger)+pinBytes);
            indexCapacity_=slots;continue;
        }
        SortPins();
        for(uint64_t i=0;i<count_;++i)KMutexLock(&pins_[i].file->gate);
        KMutexLock(&space->mutation);
        if(space->generation==generation) { space_=space;return true; }
        KMutexUnlock(&space->mutation);
        for(uint64_t i=count_;i>0;--i)KMutexUnlock(&pins_[i-1].file->gate);
        ReleasePins();
    }
}
void FileMappingGateSet::Unlock() {
    if(!space_)return;
    KMutexUnlock(&space_->mutation);space_=nullptr;
    for(uint64_t i=count_;i>0;--i)KMutexUnlock(&pins_[i-1].file->gate);
    ReleasePins();
}
void FileMappingGateSet::Finish() {
    Unlock();ReleasePins();if(pins_!=local_)kfree(pins_);pins_=local_;capacity_=8;index_=nullptr;indexCapacity_=0;
}
}

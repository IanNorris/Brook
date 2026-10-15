#include "mapping_transaction.h"
#include "physical_memory.h"
#include "heap.h"
#include "process.h"
#include "scheduler.h"
#include "apic.h"
#include "panic.h"
#include "string.h"

namespace brook {
MappingTransaction::MappingTransaction(Process* actor, AddressSpaceState* space)
    : actor_(actor), space_(space) {
    if (!actor_ || !space_ || space_->kernel)
        KernelPanic("mapping: transaction requires a user AS and process context");
#ifndef BROOK_HOST_TEST
    if (actor_ != ProcessCurrent()) KernelPanic("mapping: actor is not the executing task");
#endif
    // Enter before retaining resources: fatal cancellation cannot discard the
    // operation's stack while preparation or retirement blocks.
    SchedulerKernelRegionEnter(actor_);
    AddressSpaceRef(space_);
}
MappingTransaction::~MappingTransaction() {
    Unlock();
    if (shootdown_) TlbShootdownFull(space_->pageTable.pml4.raw(), space_->tlbCpuMask);
    for (uint64_t i = 0; i < retiredCount_; ++i)
        if (retired_[i].managed) PmmUnrefPage(VmmLeafPhysical(retired_[i].raw));
    for (uint64_t i = 0; i < backingCount_; ++i)
        backings_[i].release(backings_[i].pointer);
    for (uint64_t i = 0; i < freshCapacity_; ++i)
        if (fresh_[i].physical && !fresh_[i].published) PmmUnrefPage(fresh_[i].physical);
    kfree(retired_); kfree(fresh_); kfree(backings_);
    AddressSpaceUnref(space_);
    SchedulerKernelRegionLeave(actor_);
}
static void* AllocateRecords(uint64_t count, uint64_t size) {
    if (!count) return nullptr;
    if (count > ~uint64_t{0} / size) return nullptr;
    void* memory = kmalloc(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
bool MappingTransaction::Prepare(uint64_t retiredCapacity, uint64_t freshCapacity,
                                 uint64_t backingCapacity) {
    if (locked_ || prepared_) KernelPanic("mapping: prepare under lock or repeated prepare");
    prepared_ = true;
    retired_ = static_cast<RetiredLeaf*>(AllocateRecords(retiredCapacity, sizeof(RetiredLeaf)));
    fresh_ = static_cast<FreshPage*>(AllocateRecords(freshCapacity, sizeof(FreshPage)));
    backings_ = static_cast<Backing*>(AllocateRecords(backingCapacity, sizeof(Backing)));
    // Capacity is assigned only for successfully allocated storage, including
    // partial failures: the destructor never indexes a null allocation.
    retiredCapacity_ = retired_ ? retiredCapacity : 0;
    freshCapacity_ = fresh_ ? freshCapacity : 0;
    backingCapacity_ = backings_ ? backingCapacity : 0;
    return (!retiredCapacity || retired_) && (!freshCapacity || fresh_) &&
           (!backingCapacity || backings_);
}
bool MappingTransaction::PrepareZeroed(MemTag tag, uint16_t pid) {
    if (locked_ || !prepared_ || tag == MemTag::Device)
        KernelPanic("mapping: invalid managed-page preparation");
    for (uint64_t i = 0; i < freshCapacity_; ++i) {
        if (fresh_[i].physical) KernelPanic("mapping: duplicate prepared page");
        auto physical = PmmAllocPage(tag, pid);
        if (!physical) return false;
        fresh_[i].physical = physical;
        memset(reinterpret_cast<void*>(PhysToVirt(physical).raw()), 0, 4096);
    }
    return true;
}
void MappingTransaction::Lock() {
    if (locked_) KernelPanic("mapping: recursive mutation lock");
    KMutexLock(&space_->mutation); locked_ = true; ++lockEpoch_;
}
void MappingTransaction::Unlock() {
    if (locked_) { locked_ = false; KMutexUnlock(&space_->mutation); }
}
void MappingTransaction::Changed(bool requiresShootdown) {
    if (!locked_) KernelPanic("mapping: change without mutation lock");
    if (!++space_->generation) KernelPanic("mapping: generation exhausted");
    shootdown_ |= requiresShootdown;
}
bool MappingTransaction::Borrowed(VirtualAddress address, uint64_t leaf) const {
    if (((leaf & PTE_TAG_MASK) >> PTE_TAG_SHIFT) == static_cast<uint64_t>(MemTag::Device))
        return true;
    for (const auto& map : space_->memfdMaps)
        if (map.length && map.mfd && address.raw() >= map.vaddr &&
            address.raw() - map.vaddr < map.length) return true;
    return false;
}
bool MappingTransaction::Detach(VirtualAddress address) {
    if (!locked_) KernelPanic("mapping: detach without mutation lock");
    if (retiredCount_ == retiredCapacity_) return false;
    uint64_t old = VmmDetachPage(space_->pageTable, address);
    if (old) {
        retired_[retiredCount_++] = {old, !Borrowed(address, old)};
        Changed();
    }
    return true;
}
VmmMapResult MappingTransaction::Publish(uint64_t index, VirtualAddress address,
                                        uint64_t flags, MemTag tag, uint16_t pid) {
    if (!locked_ || tag == MemTag::Device || index >= freshCapacity_ ||
        !fresh_[index].physical || fresh_[index].published)
        KernelPanic("mapping: invalid prepared publication");
    auto result = VmmTryMapPage(space_->pageTable, address, fresh_[index].physical, flags, tag, pid);
    if (result == VmmMapResult::Mapped) {
        fresh_[index].published = true;
        fresh_[index].publishedAt = address;
        fresh_[index].publicationEpoch = lockEpoch_;
        Changed(false);
    }
    return result;
}
void MappingTransaction::RollbackPublished() {
    if (!locked_) KernelPanic("mapping: rollback without mutation lock");
    for (uint64_t i = 0; i < freshCapacity_; ++i) {
        if (!fresh_[i].published || fresh_[i].settled) continue;
        if (fresh_[i].publicationEpoch != lockEpoch_)
            KernelPanic("mapping: rollback after dropping the publication lock");
        if (retiredCount_ == retiredCapacity_) KernelPanic("mapping: rollback capacity");
        auto old = VmmDetachPageIfMatches(space_->pageTable, fresh_[i].publishedAt, fresh_[i].physical);
        if (old) {
            retired_[retiredCount_++] = {old, true};
            Changed();
        }
        fresh_[i].settled = true;
        // The original fresh ref transferred to the PTE. It either belongs to
        // this retirement record or to the writer that replaced that PTE.
    }
}
bool MappingTransaction::RetireBacking(void* backing, void (*release)(void*)) {
    if (!locked_ || !backing || !release || backingCount_ == backingCapacity_) return false;
    backings_[backingCount_++] = {backing, release};
    shootdown_ = true;
    return true;
}
}

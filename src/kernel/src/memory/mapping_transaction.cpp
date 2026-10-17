#include "mapping_transaction.h"
#include "physical_memory.h"
#include "heap.h"
#include "process.h"
#include "scheduler.h"
#include "apic.h"
#include "panic.h"
#include "string.h"
#include "file_object.h"

namespace brook {
MappingTransaction::MappingTransaction(Process* actor, AddressSpaceState* space,MappingBacking* incoming,MappingBacking* onlyBacking)
    : actor_(actor), space_(space),incoming_(incoming),onlyBacking_(onlyBacking) {
    if (!actor_ || !space_ || space_->kernel)
        KernelPanic("mapping: transaction requires a user AS and process context");
#ifndef BROOK_HOST_TEST
    if (actor_ != ProcessCurrent()) KernelPanic("mapping: actor is not the executing task");
#endif
    // Enter before retaining resources: fatal cancellation cannot discard the
    // operation's stack while preparation or retirement blocks.
    SchedulerKernelRegionEnter(actor_);
    AddressSpaceRef(space_);MappingBackingRef(incoming_);MappingBackingRef(onlyBacking_);
}
MappingTransaction::~MappingTransaction() {
    Unlock();
    if (shootdown_) TlbShootdownFull(space_->pageTable.pml4.raw(), space_->tlbCpuMask);
    for (uint64_t i = 0; i < retiredCount_; ++i)
        if (retired_[i].managed) PmmUnrefPage(VmmLeafPhysical(retired_[i].raw));
    for (uint64_t i = 0; i < backingCount_; ++i)
        backings_[i].release(backings_[i].pointer);
    for (uint64_t i = 0; i < freshCapacity_; ++i)
        if (fresh_[i].physical && fresh_[i].managed && !fresh_[i].published) PmmUnrefPage(fresh_[i].physical);
    // Finish explicitly before leaving cancellation protection. C++ member
    // destructors otherwise run after this body, too late to own finalizers.
    ranges_.Finish();
    MappingBackingUnref(snapshotBacking_);
    kfree(retired_); kfree(fresh_); kfree(backings_);
    fileGates_.Finish();
    MappingBackingUnref(incoming_);MappingBackingUnref(onlyBacking_);
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
        fresh_[i].managed = true;
        memset(reinterpret_cast<void*>(PhysToVirt(physical).raw()), 0, 4096);
    }
    return true;
}
bool MappingTransaction::StagePage(uint64_t index, PhysicalAddress physical, bool managed) {
    if (locked_) KernelPanic("mapping: staging a page under lock");
    if (index >= freshCapacity_ || fresh_[index].physical || !physical || (physical.raw() & 4095)) return false;
    fresh_[index].physical = physical; fresh_[index].managed = managed; return true;
}
PhysicalAddress MappingTransaction::PreparedPage(uint64_t index) const {
    return index < freshCapacity_ ? fresh_[index].physical : PhysicalAddress{};
}
bool MappingTransaction::Lock() {
    if (locked_) KernelPanic("mapping: recursive mutation lock");
    if(!fileGates_.Lock(space_,incoming_,onlyBacking_))return false;
    locked_ = true; ++lockEpoch_;return true;
}
void MappingTransaction::Unlock() {
    if (locked_) { locked_ = false; fileGates_.Unlock(); }
}
void MappingTransaction::Changed(bool requiresShootdown) {
    if (!locked_) KernelPanic("mapping: change without mutation lock");
    if (!++space_->generation) KernelPanic("mapping: generation exhausted");
    shootdown_ |= requiresShootdown;
}
bool MappingTransaction::Borrowed(VirtualAddress address, uint64_t) const {
    if (const auto* range = MappingRangeFind(space_->ranges,address.raw()))
        return range->leafOwnership == MappingLeafOwnership::Borrowed;
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
                                        uint64_t flags, MemTag tag, uint16_t pid, bool accessible) {
    if (!locked_ || index >= freshCapacity_ ||
        !fresh_[index].physical || fresh_[index].published)
        KernelPanic("mapping: invalid prepared publication");
    if (tag == MemTag::Device && fresh_[index].managed)
        KernelPanic("mapping: managed prepared page cannot use borrowed Device tag");
    auto result = VmmTryMapPage(space_->pageTable, address, fresh_[index].physical, flags, tag, pid, accessible);
    if (result == VmmMapResult::Mapped) {
        fresh_[index].published = true;
        fresh_[index].publishedAt = address;
        fresh_[index].publicationEpoch = lockEpoch_;
        // Population of a prepared leaf does not change topology/protection.
        // In particular, it must not invalidate a fault's adjacent-page snapshot.
        // The caller records any actual range/break metadata change separately.
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
            retired_[retiredCount_++] = {old, fresh_[i].managed};
            Changed();
        }
        fresh_[i].settled = true;
        // The original fresh ref transferred to the PTE. It either belongs to
        // this retirement record or to the writer that replaced that PTE.
    }
}
bool MappingTransaction::RetireBacking(void* backing, void (*release)(void*)) {
    if (!HoldBacking(backing,release)) return false;
    shootdown_ = true;
    return true;
}
bool MappingTransaction::HoldBacking(void* backing, void (*release)(void*)) {
    if (!locked_ || !backing || !release || backingCount_ == backingCapacity_) return false;
    backings_[backingCount_++] = {backing,release}; return true;
}
bool MappingTransaction::RetireFrame(PhysicalAddress physical) {
    if (!locked_ || !physical || retiredCount_ == retiredCapacity_) return false;
    retired_[retiredCount_++] = {physical.raw(),true}; shootdown_ = true; return true;
}
void MappingTransaction::RequireShootdown() {
    if (!locked_) KernelPanic("mapping: shootdown requested without lock");
    shootdown_ = true;
}
bool MappingTransaction::PrepareRanges(uint64_t nodes) {
    if (locked_) KernelPanic("mapping: range preparation under lock");
    return ranges_.Prepare(nodes);
}
bool MappingTransaction::RemoveMappingWithRetirer(uint64_t begin,uint64_t end,VmmRetireLeaf retire,void* context) {
    if (!locked_) KernelPanic("mapping: external retirement without lock");
    if (!ranges_.CanReplace(space_->ranges,begin,end)) return false;
    uint64_t detached=0;
    if (!VmmDetachRange(space_->pageTable,begin,end,~uint64_t{0},retire,context,&detached)) return false;
    if (!ranges_.Replace(space_->ranges,begin,end)) KernelPanic("mapping: external retirement preflight changed");
    Changed(detached!=0);return true;
}
bool MappingTransaction::ReplaceMapping(uint64_t begin, uint64_t end, const MappingRange* replacement) {
    if (!locked_) KernelPanic("mapping: range replacement without lock");
    if (!ranges_.CanReplace(space_->ranges,begin,end,replacement)) return false;
    uint64_t detached = 0;
    if (!VmmDetachRange(space_->pageTable,begin,end,retiredCapacity_-retiredCount_,
                         CaptureDetachedLeaf,this,&detached)) return false;
    if (!ranges_.Replace(space_->ranges,begin,end,replacement))
        KernelPanic("mapping: range preflight changed under lock");
    Changed(detached != 0); return true;
}
bool MappingTransaction::CanReplaceRange(uint64_t begin, uint64_t end, const MappingRange* replacement) const {
    if (!locked_) KernelPanic("mapping: range preflight without lock");
    return ranges_.CanReplace(space_->ranges,begin,end,replacement);
}
void MappingTransaction::CaptureDetachedLeaf(VirtualAddress address, uint64_t leaf, void* context) {
    auto* operation = static_cast<MappingTransaction*>(context);
    operation->retired_[operation->retiredCount_++] = {leaf,!operation->Borrowed(address,leaf)};
}
bool MappingTransaction::RangeLeafCount(uint64_t begin, uint64_t end, uint64_t& count) const {
    if (!locked_) KernelPanic("mapping: range count without lock");
    return VmmCountRangeLeaves(space_->pageTable,begin,end,&count);
}
bool MappingTransaction::InsertRange(const MappingRange& range) {
    if (!locked_) KernelPanic("mapping: range insertion without lock");
    if (!ranges_.Insert(space_->ranges,range)) return false;
    Changed(false); return true;
}
bool MappingTransaction::ExtendRange(uint64_t identity,uint64_t end) {
    if (!locked_) KernelPanic("mapping: extension without lock");
    if (!ranges_.Extend(space_->ranges,identity,end)) return false;
    Changed(false);return true;
}
bool MappingTransaction::ProtectRange(uint64_t begin, uint64_t end, uint32_t protection) {
    if (!locked_) KernelPanic("mapping: range protection without lock");
    if (!ranges_.Protect(space_->ranges,begin,end,protection)) return false;
    Changed(); return true;
}
bool MappingTransaction::PrivateLeaf(VirtualAddress address, uint64_t, void* context) {
    auto* operation = static_cast<MappingTransaction*>(context);
    const auto* range = MappingRangeFind(operation->space_->ranges,address.raw());
    return range && range->sharing == MappingSharing::Private && range->leafOwnership == MappingLeafOwnership::Managed;
}
bool MappingTransaction::ProtectMapping(uint64_t begin, uint64_t end, uint32_t protection) {
    if (!locked_) KernelPanic("mapping: protection without lock");
    if (!ranges_.CanProtect(space_->ranges,begin,end,protection)) return false;
    if (!VmmProtectRange(space_->pageTable,begin,end,protection,PrivateLeaf,this)) return false;
    if (!ranges_.Protect(space_->ranges,begin,end,protection)) KernelPanic("mapping: protection preflight changed under lock");
    Changed(); return true;
}
static void ReleaseMappingBacking(void* backing) { MappingBackingUnref(static_cast<MappingBacking*>(backing)); }
bool MappingTransaction::CaptureMapping(uint64_t address, MappingFaultSnapshot& snapshot) {
    if (!locked_) KernelPanic("mapping: range snapshot without lock");
    const auto* range = MappingRangeFind(space_->ranges,address);
    if (!range) return false;
    if (range->backing) {
        if (!snapshotBacking_) { MappingBackingRef(range->backing);snapshotBacking_=range->backing; }
        else if (snapshotBacking_ != range->backing) {
            if (!HoldBacking(range->backing,ReleaseMappingBacking)) return false;
            MappingBackingRef(range->backing);
        }
    }
    snapshot = {*range,space_->identity,space_->generation,
        range->backing && range->backing->file ? range->backing->file->generation : 0}; return true;
}
bool MappingTransaction::CaptureRange(uint64_t address, uint32_t access, MappingFaultSnapshot& snapshot) {
    if (!locked_) KernelPanic("mapping: range snapshot without lock");
    const auto* range=MappingRangeFind(space_->ranges,address);
    if (!range || !access || (access & ~range->protection)) return false;
    return CaptureMapping(address,snapshot);
}
bool MappingTransaction::ValidateMapping(const MappingFaultSnapshot& snapshot, uint64_t address) const {
    if (!locked_) KernelPanic("mapping: range validation without lock");
    if (snapshot.addressSpaceIdentity != space_->identity || snapshot.generation != space_->generation) return false;
    const auto* range = MappingRangeFind(space_->ranges,address);
    const auto& old = snapshot.range;
    return range && range->identity == old.identity && range->begin == old.begin && range->end == old.end &&
           range->offset == old.offset && range->backing == old.backing &&
           range->protection == old.protection && range->maximumProtection == old.maximumProtection &&
           range->kind == old.kind && range->sharing == old.sharing && range->leafOwnership == old.leafOwnership &&
           (!range->backing || !range->backing->file || range->backing->file->generation==snapshot.fileGeneration);
}
bool MappingTransaction::ValidateRange(const MappingFaultSnapshot& snapshot, uint64_t address, uint32_t access) const {
    return access && !(access & ~snapshot.range.protection) && ValidateMapping(snapshot,address);
}
MappingFaultCommit MappingTransaction::PublishFault(const MappingFaultSnapshot& snapshot, uint64_t index,
                                                    VirtualAddress address, uint32_t access, bool copyOnWrite) {
    if (!ValidateRange(snapshot,address.raw(),access)) return MappingFaultCommit::Stale;
    if (copyOnWrite && (snapshot.range.sharing != MappingSharing::Private ||
        snapshot.range.leafOwnership != MappingLeafOwnership::Managed)) return MappingFaultCommit::Failed;
    uint64_t flags = VMM_USER;
    if (!(snapshot.range.protection & 4)) flags |= VMM_NO_EXEC;
    if (snapshot.range.protection & 2) flags |= copyOnWrite ? PTE_COW_BIT : VMM_WRITABLE;
    const bool borrowed = snapshot.range.leafOwnership == MappingLeafOwnership::Borrowed;
    if (index >= freshCapacity_ || fresh_[index].managed == borrowed) return MappingFaultCommit::Failed;
    MemTag tag = borrowed && snapshot.range.kind == MappingKind::Device ? MemTag::Device : MemTag::User;
    auto result = Publish(index,address,flags,tag,space_->allocationPid);
    if (result == VmmMapResult::Mapped) return MappingFaultCommit::Mapped;
    if (result != VmmMapResult::Occupied) return MappingFaultCommit::Failed;
    const uint64_t leaf = VmmReadLeaf(space_->pageTable,address);
    if ((leaf & (VMM_PRESENT | VMM_USER)) != (VMM_PRESENT | VMM_USER) ||
        ((access & 4) && (leaf & VMM_NO_EXEC)) ||
        ((access & 2) && !(leaf & (VMM_WRITABLE | PTE_COW_BIT)))) return MappingFaultCommit::Failed;
    return MappingFaultCommit::Existing;
}
FileMappingInvalidation::FileMappingInvalidation(Process* actor, AddressSpaceState* space,
                                               MappingBacking* backing)
    : actor_(actor), space_(space), backing_(backing) {
    if (!actor || !space || space->kernel || !backing || !backing->file)
        KernelPanic("mapping: invalid target-file invalidation");
#ifndef BROOK_HOST_TEST
    if (actor != ProcessCurrent()) KernelPanic("mapping: invalid invalidation actor");
#endif
    SchedulerKernelRegionEnter(actor_);
    AddressSpaceRef(space_);MappingBackingRef(backing_);
    RequireTargetGate();
}
void FileMappingInvalidation::RequireTargetGate() const {
#ifndef BROOK_HOST_TEST
    auto* gate=&backing_->file->gate;
    if (!gate->locked || gate->owner!=actor_)
        KernelPanic("mapping: target file gate not owned");
#endif
}
FileMappingInvalidation::~FileMappingInvalidation() {
    Acknowledge();
    for (uint64_t i=0;i<count_;++i)
        if (leaves_[i].managed) PmmUnrefPage(VmmLeafPhysical(leaves_[i].raw));
    for (uint64_t i=0;i<boundaryCount_;++i)
        if (boundaries_[i]) PmmUnrefPage(boundaries_[i]);
    kfree(boundaries_);
    kfree(leaves_);MappingBackingUnref(backing_);AddressSpaceUnref(space_);
    SchedulerKernelRegionLeave(actor_);
}
bool FileMappingInvalidation::Prepare(uint64_t leafCapacity) {
    if (locked_ || prepared_) KernelPanic("mapping: invalid target retirement preparation");
    prepared_=true;
    leaves_=static_cast<Leaf*>(AllocateRecords(leafCapacity,sizeof(Leaf)));
    capacity_=leaves_?leafCapacity:0;
    return !leafCapacity || leaves_;
}
bool FileMappingInvalidation::PreparePrivateRestore(uint64_t validPrefix) {
    RequireTargetGate();
    if (!prepared_ || locked_ || count_ || acknowledged_ || restoreRequested_)
        KernelPanic("mapping: invalid private restore preparation");
    restoreRequested_=true;validPrefix_=validPrefix;
    uint64_t needed=0;
    if (validPrefix&4095) {
        const uint64_t boundary=validPrefix&~uint64_t{4095};
        LockTargetFileAlreadyHeld();
        for (auto* node=space_->ranges.head;node;node=node->next) {
            const auto& range=node->range;
            if (!range.backing || range.backing->file!=backing_->file ||
                range.sharing!=MappingSharing::Private || range.leafOwnership!=MappingLeafOwnership::Managed ||
                boundary<range.offset || boundary-range.offset>=range.end-range.begin) continue;
            const uint64_t raw=VmmReadLeaf(space_->pageTable,VirtualAddress(range.begin+boundary-range.offset));
            if (VmmLeafBacked(raw) && !FileObjectIsCanonicalLocked(backing_->file,boundary/4096,VmmLeafPhysical(raw)))
                ++needed;
        }
        Unlock();
    }
    if (needed>capacity_) return false;
    boundaries_=static_cast<PhysicalAddress*>(AllocateRecords(needed,sizeof(PhysicalAddress)));
    if (needed && !boundaries_) return false;
    for (;boundaryCount_<needed;++boundaryCount_) {
        boundaries_[boundaryCount_]=PmmAllocPage(MemTag::User,space_->allocationPid);
        if (!boundaries_[boundaryCount_]) return false;
    }
    restorePrepared_=true;return true;
}
void FileMappingInvalidation::Acknowledge() {
    RequireTargetGate();Unlock();
    if (shootdown_) {
        TlbShootdownFull(space_->pageTable.pml4.raw(),space_->tlbCpuMask);
        shootdown_=false;
    }
    acknowledged_=true;
}
void FileMappingInvalidation::LockTargetFileAlreadyHeld() {
    if (locked_) KernelPanic("mapping: recursive target AS lock");
    RequireTargetGate();KMutexLock(&space_->mutation);locked_=true;
}
void FileMappingInvalidation::Unlock() {
    if (locked_) { locked_=false;KMutexUnlock(&space_->mutation); }
}
static bool FileInterval(const MappingRange& range,FileObject* file,uint64_t begin,uint64_t end,
                         uint64_t& virtualBegin,uint64_t& virtualEnd) {
    if (!range.backing || range.backing->file!=file) return false;
    const uint64_t rangeEnd=range.offset+(range.end-range.begin);
    if (begin>=rangeEnd || end<=range.offset) return false;
    const uint64_t overlapBegin=begin>range.offset?begin:range.offset;
    const uint64_t overlapEnd=end<rangeEnd?end:rangeEnd;
    virtualBegin=range.begin+(overlapBegin-range.offset);
    virtualEnd=range.begin+(overlapEnd-range.offset);
    return true;
}
bool FileMappingInvalidation::Count(uint64_t begin,uint64_t end,uint64_t& count) const {
    if (!locked_) KernelPanic("mapping: target count without AS lock");
    RequireTargetGate();count=0;
    if (begin>=end || ((begin|end)&4095)) return false;
    for (auto* node=space_->ranges.head;node;node=node->next) {
        uint64_t first,last,leaves=0;
        if (!FileInterval(node->range,backing_->file,begin,end,first,last)) continue;
        if (!VmmCountRangeLeaves(space_->pageTable,first,last,&leaves) || leaves>~uint64_t{0}-count)
            return false;
        count+=leaves;
    }
    return true;
}
void FileMappingInvalidation::Capture(VirtualAddress address,uint64_t raw,void* opaque) {
    auto* job=static_cast<FileMappingInvalidation*>(opaque);
    const auto* range=MappingRangeFind(job->space_->ranges,address.raw());
    if (!range || !range->backing || range->backing->file!=job->backing_->file || job->count_==job->capacity_)
        KernelPanic("mapping: invalid target retirement capture");
    const uint64_t offset=range->offset+address.raw()-range->begin;
    const bool managed=range->leafOwnership==MappingLeafOwnership::Managed;
    const bool privateData=managed && range->sharing==MappingSharing::Private &&
        !FileObjectIsCanonicalLocked(job->backing_->file,offset/4096,VmmLeafPhysical(raw));
    uint64_t boundary=~uint64_t{0};
    if (job->restorePrepared_ && privateData && (job->validPrefix_&4095) &&
        offset==(job->validPrefix_&~uint64_t{4095})) {
        if (job->boundaryUsed_==job->boundaryCount_) KernelPanic("mapping: unprepared private boundary");
        boundary=job->boundaryUsed_++;
    }
    job->leaves_[job->count_++]={address.raw(),offset,raw,boundary,managed,privateData};
}
bool FileMappingInvalidation::Detach(uint64_t begin,uint64_t end) {
    uint64_t needed=0;
    if (!prepared_ || acknowledged_ || (restoreRequested_ && !restorePrepared_) ||
        !Count(begin,end,needed) || needed>capacity_-count_) return false;
    if (!needed) return true;
    if (space_->generation==~uint64_t{0}) KernelPanic("mapping: generation exhausted");
    for (auto* node=space_->ranges.head;node;node=node->next) {
        uint64_t first,last,detached=0;
        if (!FileInterval(node->range,backing_->file,begin,end,first,last)) continue;
        if (!VmmDetachRange(space_->pageTable,first,last,capacity_-count_,Capture,this,&detached))
            KernelPanic("mapping: target detach changed after preflight");
    }
    ++space_->generation;shootdown_=true;return true;
}
bool FileMappingInvalidation::RestorePrivate() {
    RequireTargetGate();
    if (!locked_ || !acknowledged_ || !restorePrepared_ || restored_) return false;
    // File gate excludes range changes and leaf publication for this inode.
    // Validate every destination before restoring the first reference.
    uint64_t restoring=0;
    for (uint64_t i=0;i<count_;++i) {
        const auto& leaf=leaves_[i];
        if (!leaf.privateData || leaf.offset>=validPrefix_) continue;
        const auto* range=MappingRangeFind(space_->ranges,leaf.address);
        if (!range || !range->backing || range->backing->file!=backing_->file ||
            range->sharing!=MappingSharing::Private || range->leafOwnership!=MappingLeafOwnership::Managed ||
            range->offset+leaf.address-range->begin!=leaf.offset ||
            VmmReadLeaf(space_->pageTable,VirtualAddress(leaf.address)))
            KernelPanic("mapping: private restore destination changed");
        ++restoring;
    }
    if (restoring && space_->generation==~uint64_t{0}) KernelPanic("mapping: generation exhausted");
    for (uint64_t i=0;i<count_;++i) {
        auto& leaf=leaves_[i];
        if (!leaf.privateData || leaf.offset>=validPrefix_) continue;
        PhysicalAddress physical=VmmLeafPhysical(leaf.raw);
        if (leaf.boundary!=~uint64_t{0}) {
            physical=boundaries_[leaf.boundary];
            const uint64_t prefix=validPrefix_-leaf.offset;
            auto* bytes=reinterpret_cast<uint8_t*>(PhysToVirt(physical).raw());
            memcpy(bytes,reinterpret_cast<void*>(PhysToVirt(VmmLeafPhysical(leaf.raw)).raw()),prefix);
            memset(bytes+prefix,0,4096-prefix);
        }
        const uint64_t flags=leaf.raw&(0xffULL|VMM_NO_EXEC|PTE_COW_BIT);
        const auto tag=static_cast<MemTag>((leaf.raw&PTE_TAG_MASK)>>PTE_TAG_SHIFT);
        const auto pid=static_cast<uint16_t>((leaf.raw&PTE_PID_MASK)>>PTE_PID_SHIFT);
        if (VmmTryMapPage(space_->pageTable,VirtualAddress(leaf.address),physical,flags,tag,pid,
                         (leaf.raw&VMM_PRESENT)!=0)!=VmmMapResult::Mapped)
            KernelPanic("mapping: prepared private restore failed");
        if (leaf.boundary!=~uint64_t{0}) boundaries_[leaf.boundary]=PhysicalAddress{};
        else leaf.managed=false; // detached reference transferred back to PTE
    }
    if (restoring) ++space_->generation;
    restored_=true;return true;
}

}

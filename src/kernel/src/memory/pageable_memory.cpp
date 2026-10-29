#include "pageable_memory.h"
#include "apic.h"
#include "pageable_application_policy.h"
#include "page_store.h"
#include "compressed_page_pool.h"
#include "lz4.h"
#include "fault_frame_reserve.h"
#include "file_page_cache.h"
#include "user_mapping.h"
#include "physical_memory.h"
#include "device.h"
#include "fw_cfg.h"
#include "heap.h"
#include "process.h"
#include "process_user_namespace.h"
#include "user_access_scope.h"
#include "user_copy.h"
#include "serial.h"
#include "string.h"
#include "sync/kmutex.h"
#include "scheduler.h"
#include "panic.h"

namespace brook {
namespace {
PageStore store;
KMutex storeLock;
bool ready=false;
bool applicationClock=false;
PageableApplicationPolicy applicationPolicy;
// Root-readable diagnostic totals survive worker exit. Held is a sampled
// per-AS maximum, not an exact process or whole-system RSS high-water mark.
uint64_t applicationStats[19]{};
uint64_t applicationBarriers[512]{};
void ApplicationCount(AddressSpaceState* space,unsigned counter,uint64_t amount=1) {
    if(space->pagingApplication)__atomic_fetch_add(&applicationStats[counter],amount,__ATOMIC_RELAXED);
}
void ApplicationObserveHeld(AddressSpaceState* space,uint64_t held) {
    if(!space->pagingApplication)return;
    uint64_t old=__atomic_load_n(&applicationStats[9],__ATOMIC_RELAXED);
    while(old<held && !__atomic_compare_exchange_n(&applicationStats[9],&old,held,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
FaultFrameReserve faultReserve;
IrqSpinLock reserveLock;
unsigned failOrdinaryAllocations=0;
bool refillEnabled=true,refillStarted=false;
uint64_t reserveRefilled=0,reserveLowHeadroom=0,reserveAllocationFailures=0;
constexpr uint64_t ReserveHeadroomPages=64*1024*1024/4096;
extern "C" void SchedulerSleepMs(uint32_t ms);

unsigned ReserveSize() {
    auto flags=IrqSpinLockAcquire(&reserveLock);auto size=faultReserve.Size();
    IrqSpinLockRelease(&reserveLock,flags);return size;
}
// Only the boot-enabled worker calls this, with no AS/store/reserve lock held.
// Each attempt owns one new KernelData reference until publication or release.
// The bound holds even if other faults consume/refill the pool concurrently.
void RefillReserve() {
    for(unsigned attempt=0;attempt<FaultFrameReserve::Capacity;++attempt) {
        if(ReserveSize()==FaultFrameReserve::Capacity)return;
        if(PmmGetFreePageCount()<=ReserveHeadroomPages) {
            __atomic_fetch_add(&reserveLowHeadroom,1,__ATOMIC_RELAXED);return;
        }
        auto frame=PmmAllocPage(MemTag::KernelData,KernelPid);
        if(!frame) {__atomic_fetch_add(&reserveAllocationFailures,1,__ATOMIC_RELAXED);return;}
        auto flags=IrqSpinLockAcquire(&reserveLock);
        const bool retained=faultReserve.Push(frame);
        IrqSpinLockRelease(&reserveLock,flags);
        if(retained)__atomic_fetch_add(&reserveRefilled,1,__ATOMIC_RELAXED);
        else PmmUnrefPage(frame); // a concurrent pageout may have filled it
    }
}
void ReserveRefillWorker(void*) {
    for(;;) {RefillReserve();SchedulerSleepMs(100);}
}
// Consumes the sole detached User reference, whether pooled or freed. Never
// called until a pageout's TLB acknowledgement and successful storage, or a
// failed fault-in transaction has dropped its staged reference.
void ReturnReserveFrame(PhysicalAddress frame) {
    if(ReserveSize()==FaultFrameReserve::Capacity){PmmUnrefPage(frame);return;}
    if(!PmmReclassifyPrivateFrame(frame,MemTag::User,MemTag::KernelData,KernelPid))
        KernelPanic("paging: invalid reserve return ownership");
    auto flags=IrqSpinLockAcquire(&reserveLock);bool retained=faultReserve.Push(frame);
    IrqSpinLockRelease(&reserveLock,flags);
    if(!retained)PmmUnrefPage(frame); // another pageout may have filled the pool
}
struct FaultLifetime {
    Process* actor;AddressSpaceState* space;
    FaultLifetime(Process* p,AddressSpaceState* s):actor(p),space(s){SchedulerKernelRegionEnter(actor);AddressSpaceRef(space);}
    ~FaultLifetime(){AddressSpaceUnref(space);SchedulerKernelRegionLeave(actor);}
};
// Declared before the transaction: failed fresh-page cleanup runs first.
// Checkout retains one ref; StagePage owns an additional ref independently.
struct ReserveCheckout {
    PhysicalAddress physical;bool published=false;
    bool Take(uint16_t pid) {
        auto flags=IrqSpinLockAcquire(&reserveLock);physical=faultReserve.Pop();
        IrqSpinLockRelease(&reserveLock,flags);
        if(!physical)return false;
        if(!PmmReclassifyPrivateFrame(physical,MemTag::KernelData,MemTag::User,pid))
            KernelPanic("paging: invalid reserve checkout ownership");
        return true;
    }
    ~ReserveCheckout(){if(physical){if(published)PmmUnrefPage(physical);else ReturnReserveFrame(physical);}}
};
PhysicalAddress AllocateFaultFrame(uint16_t pid) {
    auto flags=IrqSpinLockAcquire(&reserveLock);const bool fail=failOrdinaryAllocations!=0;
    if(fail)--failOrdinaryAllocations;
    IrqSpinLockRelease(&reserveLock,flags);
    return fail?PhysicalAddress{}:PmmAllocPage(MemTag::User,pid);
}
struct RamCompression {
    CompressedPagePool pool;
    alignas(8) LZ4_stream_t workspace;
    uint8_t encoded[4096],spill[4096];
    void* arena;
    uint64_t* bitmap;
    uint64_t residentBytes;
};
RamCompression* ramCompression=nullptr;
int EncodePage(void* context,const void* in,void* out,int capacity) {
    auto* ram=static_cast<RamCompression*>(context);
    return LZ4_compress_fast_extState(&ram->workspace,static_cast<const char*>(in),static_cast<char*>(out),4096,capacity,1);
}
int DecodePage(void*,const void* in,int size,void* out,int capacity) {
    return LZ4_decompress_safe(static_cast<const char*>(in),static_cast<char*>(out),size,capacity);
}
void AttachRamCompression() {
    char value[8]{};auto length=FwCfgReadFile("opt/page-store-ram-mib",value,sizeof(value));
    if(!length)return;
    unsigned mib=0;
    if(length>2){SerialPuts("paging: invalid compressed RAM capacity (raw backing retained)\n");return;}
    for(unsigned i=0;i<length;++i) {
        if(value[i]<'0' || value[i]>'9'){SerialPuts("paging: invalid compressed RAM capacity (raw backing retained)\n");return;}
        mib=mib*10+unsigned(value[i]-'0');
    }
    if(!mib)return;
    if(mib>16){SerialPuts("paging: compressed RAM capacity exceeds 16 MiB (raw backing retained)\n");return;}
    auto* ram=static_cast<RamCompression*>(kmalloc(sizeof(RamCompression)));
    if(!ram)return;
    memset(ram,0,sizeof(*ram));
    const uint32_t bytes=mib*1024*1024,words=(bytes/64+63)/64;
    ram->arena=kmalloc(bytes);ram->bitmap=static_cast<uint64_t*>(kmalloc(words*8));
    if(!ram->arena || !ram->bitmap || (reinterpret_cast<uint64_t>(&ram->workspace)&7) ||
       LZ4_sizeofState()>int(sizeof(ram->workspace)) ||
       !ram->pool.Initialize(ram->arena,bytes,ram->bitmap,words) ||
       !store.AttachCompression({&ram->pool,ram,EncodePage,DecodePage,ram->encoded,ram->spill})) {
        kfree(ram->arena);kfree(ram->bitmap);kfree(ram);return;
    }
    ram->residentBytes=bytes+uint64_t(words)*8+sizeof(*ram);ramCompression=ram;
    SerialPrintf("paging: LZ4 RAM arena=%u total_resident_bytes=%lu (plus slot metadata)\n",bytes,ram->residentBytes);
}
// Root-only one-shot failure controls for the explicitly boot-enabled test.
unsigned injectedFailure=0;
unsigned forkFailure=0;
uint64_t forkFailureSpace=0;
unsigned growthFailure=0;
uint64_t growthFailureSpace=0;
enum class State : uint8_t { Resident, Evicting, Stored, Recovery, Zero, Discarded };
struct Entry { uint64_t slot;PhysicalAddress recovery;State state; };
struct Region { uint64_t pages,heldFrames;bool accountingVisited;Entry entries[1]; };
struct Frame {
    PhysicalAddress physical;
    ~Frame(){if(physical)PmmUnrefPage(physical);}
};
struct StoreGuard { StoreGuard(){KMutexLock(&storeLock);}~StoreGuard(){KMutexUnlock(&storeLock);} };
void ReleaseSlot(uint64_t token){if(token){StoreGuard guard;store.Release(token);}}
struct Slot { uint64_t token=0;~Slot(){ReleaseSlot(token);} };
struct Backing { MappingBacking* value=nullptr;~Backing(){MappingBackingUnref(value);} };
void ReleaseRegion(void* p) {
    auto* region=static_cast<Region*>(p);
    for(uint64_t i=0;i<region->pages;++i){ReleaseSlot(region->entries[i].slot);if(region->entries[i].recovery)PmmUnrefPage(region->entries[i].recovery);}
    kfree(region);
}
int Read(void* context,uint64_t offset,void* data,uint64_t size) {
    auto* dev=static_cast<Device*>(context);
    if(injectedFailure==2){injectedFailure=0;return -5;}
    if(injectedFailure==5){injectedFailure=0;return dev->ops->read(dev,offset,data,size/2);}
    const int result=dev->ops->read(dev,offset,data,size);
    if(injectedFailure==3 && result==int(size)){injectedFailure=0;static_cast<uint8_t*>(data)[0]^=1;}
    return result;
}
int Write(void* context,uint64_t offset,const void* data,uint64_t size) {
    if(injectedFailure==1 || injectedFailure==6){injectedFailure=0;return -5;}
    if(injectedFailure==4){injectedFailure=0;return DeviceWrite(static_cast<Device*>(context),offset,data,size/2);}
    return DeviceWrite(static_cast<Device*>(context),offset,data,size);
}
bool Match(const MappingRange* range,MappingBacking* backing,uint64_t page,uint64_t index) {
    return range && range->kind==MappingKind::Pageable && range->backing==backing &&
        (range->offset+page-range->begin)/4096==index;
}
uint64_t Flags(const MappingRange& r) {
    return VMM_USER|((r.protection&2)?VMM_WRITABLE:0)|((r.protection&4)?0:VMM_NO_EXEC);
}
int Prepare(MappingTransaction& op,const UserMappingRequest&,void*) {
    return op.PrepareZeroed(MemTag::User,op.Space()->allocationPid)?0:-12;
}
int64_t Map(Process* actor,uint64_t length,bool lazy=false) {
    if(!length || (length&4095) || length>16*1024*1024)return -22;
    auto* space=actor->addressSpace;
    // Claim against CLONE_VM atomically and against fork under its mutation
    // gate. The restriction remains until exec even if allocation later fails.
    {
        MappingTransaction op(actor,space);if(!op.Lock())return -12;
        if(!space->pagingAdmission.ClaimExclusive())return -16;
    }
    uint64_t pages=length/4096;
    auto* region=static_cast<Region*>(kmalloc(sizeof(Region)+(pages-1)*sizeof(Entry)));
    if(!region)return -12;
    memset(region,0,sizeof(Region)+(pages-1)*sizeof(Entry));region->pages=pages;
    region->heldFrames=lazy?0:pages;
    if(lazy)for(uint64_t i=0;i<pages;++i)region->entries[i].state=State::Zero;
    Backing backing{MappingBackingCreate(region,ReleaseRegion)};
    if(!backing.value){kfree(region);return -12;}
    UserMappingRequest request{};
    request.range={0,length,0,0,backing.value,3,3,MappingKind::Pageable,MappingSharing::Private,MappingLeafOwnership::Managed};
    request.length=length;request.preparedPages=lazy?0:pages;request.prepare=lazy?nullptr:Prepare;
    return UserMappingCreate(actor,space,request);
}
// An optional handoff consumes the sole retained victim reference only after
// stable storage and full TLB retirement. Same-AS fault reclaim can reuse it
// directly without first discarding a frame into an already-full reserve.
int64_t PageOut(Process* actor,uint64_t page,PhysicalAddress* handoff=nullptr,bool* detachmentAcknowledged=nullptr) {
    if(detachmentAcknowledged)*detachmentAcknowledged=false;
    if(handoff)*handoff={};
    if(page&4095)return -22;
    auto* space=actor->addressSpace;
    if(!space->pagingAdmission.Exclusive())return -95;
    Slot slot;
    {StoreGuard guard;
     if(injectedFailure==8){injectedFailure=0;return -12;}
     slot.token=store.Reserve();}
    if(!slot.token)return -28;
    const uint64_t token=slot.token;
    Backing backing;Frame frame;uint64_t index=0;
    {
        MappingTransaction op(actor,space);
        if(!op.Prepare(1,0) || !op.LockFaultRange(page))return -12;
        const auto* range=MappingRangeFind(space->ranges,page);
        if(space->userAccess.CoversPage(page))return -16;
        if(!range || range->kind!=MappingKind::Pageable || !range->backing ||
           range->backing->release!=ReleaseRegion)return -95;
        auto* region=static_cast<Region*>(range->backing->object);
        index=(range->offset+page-range->begin)/4096;if(index>=region->pages)return -22;
        auto& entry=region->entries[index];
        if(entry.state!=State::Resident)return -16;
        const uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(page));
        if(!(leaf&VMM_PRESENT) || (leaf&PTE_COW_BIT))return -95;
        auto physical=VmmLeafPhysical(leaf);
        // This check is valid only in conjunction with the lifetime admission
        // and dedicated mapping-kind restrictions, NOT a general eviction pin.
        if(PmmGetRefCount(physical)!=1 || PmmGetMapCount(physical)!=1)return -16;
        PmmRefPage(physical);frame.physical=physical;
        backing.value=range->backing;MappingBackingRef(backing.value);
        entry.slot=token;entry.state=State::Evicting;
        if(!op.Detach(VirtualAddress(page))){entry.slot=0;entry.state=State::Resident;return -12;}
        slot.token=0; // region now owns the slot; local backing pin keeps it live
    } // successful detach's destructor has completed the full-AS TLB ACK
    if(detachmentAcknowledged)*detachmentAcknowledged=true;
    int result;bool rejectRollback=false;
    {StoreGuard guard;rejectRollback=injectedFailure==6;result=store.Store(token,reinterpret_cast<void*>(PhysToVirt(frame.physical).raw()));}
    // No preparation/allocation is needed for completion. Only this dedicated
    // anonymous backing can be mutated; never enter filesystem code here.
    KMutexLock(&space->mutation);
    const auto* range=MappingRangeFind(space->ranges,page);
    if(!Match(range,backing.value,page,index)) {
        KMutexUnlock(&space->mutation);return -116;
    }
    auto& entry=static_cast<Region*>(backing.value->object)->entries[index];
    if(!result) {
        entry.state=State::Stored;
        ApplicationCount(space,5);
        --static_cast<Region*>(backing.value->object)->heldFrames;
        KMutexUnlock(&space->mutation);
        if(handoff)*handoff=frame.physical;
        else ReturnReserveFrame(frame.physical);
        frame.physical={};return 4096;
    }
    slot.token=entry.slot;entry.slot=0;
    if(!rejectRollback && VmmTryMapPage(space->pageTable,VirtualAddress(page),frame.physical,Flags(*range),
                    MemTag::User,space->allocationPid,range->protection!=0)==VmmMapResult::Mapped)
        entry.state=State::Resident;
    else {
        // A failed rollback still retains the only good bytes. A later fault
        // can retry publication; backing teardown releases this reference.
        entry.recovery=frame.physical;entry.state=State::Recovery;
    }
    frame.physical={}; // transferred either to the PTE or to recovery backing
    KMutexUnlock(&space->mutation);
    return result;
}
int64_t SpillStoredPage(Process* actor,uint64_t page) {
    if(page&4095)return -22;
    MappingTransaction op(actor,actor->addressSpace);
    if(!op.LockFaultRange(page))return -12;
    MappingFaultSnapshot snapshot{};
    if(!op.CaptureRange(page,1,snapshot) || snapshot.range.kind!=MappingKind::Pageable ||
       !snapshot.range.backing || snapshot.range.backing->release!=ReleaseRegion)return -95;
    auto* region=static_cast<Region*>(snapshot.range.backing->object);
    auto index=(snapshot.range.offset+page-snapshot.range.begin)/4096;
    if(index>=region->pages || region->entries[index].state!=State::Stored)return -16;
    uint64_t token=region->entries[index].slot;op.Unlock();
    Slot pin;
    {StoreGuard guard;if(!store.Retain(token))return -116;pin.token=token;}
    {StoreGuard guard;return store.Spill(token);}
}
// AS mutation held. Split nodes share a region's accounting, not independent
// copies of it. A temporary mark gives two linear passes, including fragments
// with unrelated replacement nodes between them. Regions belong to one AS;
// its mutation gate serializes both passes and no mark survives the call.
uint64_t HeldFrames(AddressSpaceState* space) {
    uint64_t total=0;
    for(auto* node=space->ranges.head;node;node=node->next) {
        if(node->range.kind!=MappingKind::Pageable || !node->range.backing || node->range.backing->release!=ReleaseRegion)continue;
        auto* region=static_cast<Region*>(node->range.backing->object);
        if(!region->accountingVisited){region->accountingVisited=true;total+=region->heldFrames;}
    }
    for(auto* node=space->ranges.head;node;node=node->next)
        if(node->range.kind==MappingKind::Pageable && node->range.backing && node->range.backing->release==ReleaseRegion)
            static_cast<Region*>(node->range.backing->object)->accountingVisited=false;
    return total;
}
// No range/PTE pointer survives an unlock. Retain actor/AS through the sweep
// and its ACK in the outer caller. Aging does not change mapping generation.
struct AgingFlush {
    AddressSpaceState* space;bool changed=false;
    void Flush(){if(changed){TlbShootdownFull(space->pageTable.pml4.raw(),space->tlbCpuMask);changed=false;ApplicationCount(space,15);}}
    void CoveredByPageOut(){if(changed){changed=false;ApplicationCount(space,18);}}
    ~AgingFlush(){Flush();}
};
uint64_t ScanVictim(Process* actor,uint64_t excluded,bool protectRecent,bool clock,uint64_t start,bool& cleared) {
    auto* space=actor->addressSpace;
    uint64_t cursor=start;bool wrapped=false;
    for(;;) {
        uint64_t victim=0,visits=0;bool done=false;
        KMutexLock(&space->mutation);
        for(unsigned visited=0;visited<64;++visited) {
            const auto* range=MappingRangeAtOrAfter(space->ranges,cursor);
            if(!range) {
                if(!start || wrapped){done=true;break;}
                cursor=0;wrapped=true;continue;
            }
            const uint64_t page=range->begin>cursor?range->begin:cursor;
            if(wrapped && page>=start){done=true;break;}
            ++visits;
            cursor=range->kind==MappingKind::Pageable?page+4096:range->end;
            space->pagingReclaimCursor=cursor;
            if(range->kind!=MappingKind::Pageable || !range->backing ||
               range->backing->release!=ReleaseRegion || page==excluded ||
               space->userAccess.CoversPage(page))continue;
            bool recent=false;
            if(protectRecent)for(auto address:space->pagingRecentFaults)if(address==page){recent=true;break;}
            if(recent)continue;
            auto* region=static_cast<Region*>(range->backing->object);
            const auto index=(range->offset+page-range->begin)/4096;
            if(index>=region->pages || region->entries[index].state!=State::Resident)continue;
            const auto leaf=VmmReadLeaf(space->pageTable,VirtualAddress(page));
            if(!(leaf&VMM_PRESENT) || !(leaf&VMM_USER) || (leaf&PTE_COW_BIT))continue;
            const auto physical=VmmLeafPhysical(leaf);
            if(PmmGetRefCount(physical)!=1 || PmmGetMapCount(physical)!=1)continue;
            if(clock && (leaf&VMM_ACCESSED)) {
                if(VmmClearAccessedIfMatches(space->pageTable,VirtualAddress(page),physical)) {
                    cleared=true;ApplicationCount(space,14);
                }
                continue;
            }
            victim=page;break;
        }
        KMutexUnlock(&space->mutation);
        ApplicationCount(space,13,visits);
        if(victim || done)return victim; // caller owns the accumulated ACK obligation
        SchedulerYield();__asm__ volatile("sti":::"memory");
    }
}
// Two finite passes at most; only retry if the first actually cleared a leaf.
int64_t ReclaimOne(Process* actor,uint64_t excluded,bool protectRecent=false,PhysicalAddress* handoff=nullptr) {
    if(handoff)*handoff={};
    auto* space=actor->addressSpace;
    uint64_t flags;__asm__ volatile("pushfq; popq %0":"=r"(flags));
    if(!(flags&512))return -16;
    FaultLifetime lifetime(actor,space);AgingFlush aging{space};
    KMutexLock(&space->mutation);
    if(!space->pagingAdmission.Exclusive() || !space->pagingReclaimEnabled) {
        KMutexUnlock(&space->mutation);return -95;
    }
    const uint64_t start=space->pagingReclaimCursor;
    const bool clock=applicationClock && space->pagingApplication;
    KMutexUnlock(&space->mutation);
    uint64_t victim=ScanVictim(actor,excluded,protectRecent,clock,start,aging.changed);
    if(!victim && clock && aging.changed) {
        aging.Flush(); // a second scan must observe acknowledged cold samples
        ApplicationCount(space,16);
        victim=ScanVictim(actor,excluded,protectRecent,true,start,aging.changed);
    }
    if(victim) {
        bool acknowledged=false;
        const int64_t result=PageOut(actor,victim,handoff,&acknowledged);
        // PageOut's successful-detach ACK covers earlier clears in this same
        // retained AS, even if its subsequent write/rollback fails. Pre-detach
        // failures retain the RAII fallback, including exhausted backing slots.
        if(acknowledged)aging.CoveredByPageOut();
        return result;
    }
    ApplicationCount(space,17);return -12;
}
int64_t AgeOne(Process* actor,uint64_t page) {
    if(page&4095)return -22;
    auto* space=actor->addressSpace;FaultLifetime lifetime(actor,space);AgingFlush flush{space};
    KMutexLock(&space->mutation);
    const auto* range=MappingRangeFind(space->ranges,page);int64_t result=-16;
    if(space->pagingAdmission.Exclusive() && range && range->kind==MappingKind::Pageable &&
       range->backing && range->backing->release==ReleaseRegion && !space->userAccess.CoversPage(page)) {
        const auto* region=static_cast<Region*>(range->backing->object);
        const auto index=(range->offset+page-range->begin)/4096;
        const uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(page));
        if(index<region->pages && region->entries[index].state==State::Resident &&
           (leaf&(VMM_PRESENT|VMM_USER))==(VMM_PRESENT|VMM_USER) && !(leaf&PTE_COW_BIT) &&
           PmmGetRefCount(VmmLeafPhysical(leaf))==1 && PmmGetMapCount(VmmLeafPhysical(leaf))==1) {
            flush.changed=VmmClearAccessedIfMatches(space->pageTable,VirtualAddress(page),VmmLeafPhysical(leaf));
            result=flush.changed?1:0;
            if(flush.changed)ApplicationCount(space,14);
        }
    }
    KMutexUnlock(&space->mutation);return result;
}

}
bool PageableMemoryMapAnonymous(Process* actor,const UserMappingRequest& original,uint64_t flags,int64_t& result) {
    if(!ready || !actor || !actor->addressSpace ||
       !applicationPolicy.Matches(actor->exePath,original.length,original.range.protection,flags))return false;
    if(actor->addressSpace->pagingAdmission.EverShared())return false;
    const uint64_t pages=original.length/4096;
    auto* region=static_cast<Region*>(kmalloc(sizeof(Region)+(pages-1)*sizeof(Entry)));
    if(!region){result=-12;return true;}
    memset(region,0,sizeof(Region)+(pages-1)*sizeof(Entry));region->pages=pages;
    for(uint64_t i=0;i<pages;++i)region->entries[i].state=State::Zero;
    Backing backing{MappingBackingCreate(region,ReleaseRegion)};
    if(!backing.value){kfree(region);result=-12;return true;}
    auto* space=actor->addressSpace;
    {
        MappingTransaction operation(actor,space);
        if(!operation.Lock()){result=-12;return true;}
        // Unsupported/ever-shared ASs fall back before exclusive admission.
        // A successful claim is sticky even if later publication fails.
        if(!space->pagingAdmission.ClaimExclusive())return false;
        space->pagingApplication=true;space->pagingReclaimEnabled=true;
        space->pagingResidentTarget=applicationPolicy.residentPages;
    }
    auto request=original;
    request.range.kind=MappingKind::Pageable;request.range.backing=backing.value;
    request.preparedPages=0;request.prepare=nullptr;
    result=UserMappingCreate(actor,space,request);
    if(result>=0){ApplicationCount(space,0);ApplicationCount(space,1,request.length);}
    return true;
}
PageableMemoryGrowth::PageableMemoryGrowth(Process* actor,AddressSpaceState* space):actor_(actor),space_(space) {
    SchedulerKernelRegionEnter(actor_);AddressSpaceRef(space_);
}
PageableMemoryGrowth::~PageableMemoryGrowth() {
    MappingBackingUnref(replacement_);AddressSpaceUnref(space_);SchedulerKernelRegionLeave(actor_);
}
bool PageableMemoryGrowth::Prepare(uint64_t bytes) {
    if(replacement_)KernelPanic("paging: repeated growth preparation");
    if(!bytes || (bytes&4095) || bytes>16*1024*1024)return false;
    if(ready){StoreGuard guard;if(growthFailureSpace==space_->identity){failure_=growthFailure;growthFailure=0;growthFailureSpace=0;}}
    uint64_t pages=bytes/4096,size=sizeof(Region)+(pages-1)*sizeof(Entry);
    auto* region=static_cast<Region*>(kmalloc(size));if(!region)return false;
    memset(region,0,size);region->pages=pages;
    for(uint64_t i=0;i<pages;++i)region->entries[i].state=State::Zero;
    replacement_=MappingBackingCreate(region,ReleaseRegion);
    if(!replacement_){kfree(region);return false;}
    return failure_!=1;
}
bool PageableMemoryGrowth::ValidateLocked(const MappingRange& range) const {
    if(!replacement_ || failure_==2 || !space_->pagingAdmission.Exclusive() ||
       range.kind!=MappingKind::Pageable || range.sharing!=MappingSharing::Private ||
       range.leafOwnership!=MappingLeafOwnership::Managed || !range.backing || range.backing->release!=ReleaseRegion)return false;
    auto* old=static_cast<Region*>(range.backing->object);auto* next=static_cast<Region*>(replacement_->object);
    const uint64_t first=range.offset/4096,pages=(range.end-range.begin)/4096;
    if(first>old->pages || pages>old->pages-first || pages>=next->pages)return false;
    for(uint64_t i=0;i<pages;++i)
        if(old->entries[first+i].state==State::Evicting || old->entries[first+i].state==State::Discarded)return false;
    return true;
}
void PageableMemoryGrowth::TransferLocked(const MappingRange& range) {
    auto* old=static_cast<Region*>(range.backing->object);auto* next=static_cast<Region*>(replacement_->object);
    const uint64_t first=range.offset/4096,pages=(range.end-range.begin)/4096;
    // All fallible work and validation precede metadata commit. Neither array
    // can change under this AS gate, and moved/sibling slices do not alias.
    for(uint64_t i=0;i<pages;++i) {
        auto& entry=old->entries[first+i];next->entries[i]=entry;
        if(entry.state==State::Resident || entry.state==State::Recovery) {
            if(!old->heldFrames)KernelPanic("paging: growth held-frame underflow");
            --old->heldFrames;++next->heldFrames;
        }
        entry={0,{},State::Discarded};
    }
}
bool PageableMemoryGrowth::DestinationDisjointLocked(const MappingRange& source,uint64_t begin,uint64_t end) const {
    const uint64_t first=source.offset,last=first+source.end-source.begin;
    for(auto* node=space_->ranges.head;node && node->range.begin<end;node=node->next) {
        const auto& range=node->range;
        if(range.end<=begin || range.kind!=MappingKind::Pageable || range.backing!=source.backing)continue;
        const uint64_t left=range.begin>begin?range.begin:begin,right=range.end<end?range.end:end;
        const uint64_t a=range.offset+left-range.begin,b=a+right-left;
        if(first<b && a<last)return false;
    }
    return true;
}
struct PageableMemoryFork::Record {
    MappingBacking* parent;
    MappingBacking* child;
    Entry* snapshot;
    uint64_t pages,captured;
};
struct PageableMemoryFork::Span { uint64_t begin,end; };
PageableMemoryFork::PageableMemoryFork(Process* actor,AddressSpaceState* source):actor_(actor),source_(source) {
    SchedulerKernelRegionEnter(actor_);AddressSpaceRef(source_);
    if(ready && source_->pagingAdmission.Exclusive()) {
        StoreGuard guard;
        if(forkFailureSpace==source_->identity){failure_=forkFailure;forkFailure=0;forkFailureSpace=0;}
    }
}
bool PageableMemoryFork::BudgetLocked(const MappingRangeTable& table,uint64_t& regions,uint64_t& pages) {
    regions=pages=0;bool valid=true;
    for(auto* node=table.head;node;node=node->next) {
        const auto& range=node->range;
        if(range.kind!=MappingKind::Pageable)continue;
        if(!range.backing || range.backing->release!=ReleaseRegion){valid=false;continue;}
        auto* region=static_cast<Region*>(range.backing->object);
        if(!region->accountingVisited){region->accountingVisited=true;++regions;pages+=region->pages;}
    }
    for(auto* node=table.head;node;node=node->next)
        if(node->range.kind==MappingKind::Pageable && node->range.backing && node->range.backing->release==ReleaseRegion)
            static_cast<Region*>(node->range.backing->object)->accountingVisited=false;
    return valid;
}
bool PageableMemoryFork::Prepare(uint64_t regions,uint64_t pages,uint64_t ranges) {
    if(!regions)return pages==0;
    if(regions>~uint64_t{0}/(2*sizeof(uint64_t)) || regions>~uint64_t{0}/sizeof(Record) ||
       pages>~uint64_t{0}/sizeof(Entry) || ranges>~uint64_t{0}/sizeof(Span))return false;
    indexCapacity_=1;while(indexCapacity_<regions*2)indexCapacity_*=2;
    if(indexCapacity_>~uint64_t{0}/sizeof(uint64_t))return false;
    records_=static_cast<Record*>(kmalloc(regions*sizeof(Record)));
    snapshots_=kmalloc(pages*sizeof(Entry));index_=static_cast<uint64_t*>(kmalloc(indexCapacity_*sizeof(uint64_t)));
    spans_=static_cast<Span*>(kmalloc(ranges*sizeof(Span)));
    if(!records_ || !snapshots_ || !index_ || !spans_)return false;
    memset(records_,0,regions*sizeof(Record));memset(index_,0,indexCapacity_*sizeof(uint64_t));
    capacity_=regions;pageCapacity_=pages;spanCapacity_=ranges;return true;
}
PageableMemoryFork::Record* PageableMemoryFork::Find(MappingBacking* backing) const {
    if(!indexCapacity_ || !index_)return nullptr;
    uint64_t at=((reinterpret_cast<uint64_t>(backing)>>4)*0x9e3779b97f4a7c15ULL)&(indexCapacity_-1);
    for(uint64_t visited=0;visited<indexCapacity_;++visited,at=(at+1)&(indexCapacity_-1)) {
        const auto value=index_[at];if(!value)return nullptr;
        if(records_[value-1].parent==backing)return &records_[value-1];
    }
    return nullptr;
}
bool PageableMemoryFork::CaptureLocked() {
    for(auto* node=source_->ranges.head;node;node=node->next) {
        const auto& range=node->range;if(range.kind!=MappingKind::Pageable)continue;
        if(!source_->pagingAdmission.Exclusive() || !range.backing || range.backing->release!=ReleaseRegion)return false;
        if(spanCount_==spanCapacity_)return false;
        spans_[spanCount_++]={range.begin,range.end};
        if(Find(range.backing))continue;
        auto* region=static_cast<Region*>(range.backing->object);
        if(count_==capacity_ || region->pages>pageCapacity_-pageCount_)return false;
        auto& record=records_[count_];record.parent=range.backing;MappingBackingRef(record.parent);
        record.pages=region->pages;record.snapshot=static_cast<Entry*>(snapshots_)+pageCount_;pageCount_+=record.pages;
        uint64_t at=((reinterpret_cast<uint64_t>(range.backing)>>4)*0x9e3779b97f4a7c15ULL)&(indexCapacity_-1);
        while(index_[at])at=(at+1)&(indexCapacity_-1);index_[at]=++count_;
        for(uint64_t i=0;i<record.pages;++i) {
            const auto entry=region->entries[i];if(entry.state==State::Evicting)return false;
            record.snapshot[i]=entry;++record.captured;
            if(entry.recovery)PmmRefPage(entry.recovery);
        }
    }
    return failure_!=1;
}
bool PageableMemoryFork::PrepareUnlocked(uint16_t childPid,AddressSpaceState* destination) {
    // Prepare every surviving virtual fragment, including moved fragments of
    // the same backing. Empty holes need no paths and gain no mapping/access.
    for(uint64_t i=0;i<spanCount_;++i)
        if(!VmmPrepareRange(destination->pageTable,spans_[i].begin,spans_[i].end))return false;
    for(uint64_t n=0;n<count_;++n) {
        auto& record=records_[n];
        if(!record.pages || record.pages>(~uint64_t{0}-sizeof(Region))/sizeof(Entry))return false;
        const auto bytes=sizeof(Region)+(record.pages-1)*sizeof(Entry);
        auto* child=static_cast<Region*>(kmalloc(bytes));if(!child)return false;
        memset(child,0,bytes);child->pages=record.pages;
        record.child=MappingBackingCreate(child,ReleaseRegion);if(!record.child){kfree(child);return false;}
        for(uint64_t i=0;i<record.pages;++i) {
            const auto& original=record.snapshot[i];auto& entry=child->entries[i];entry.state=original.state;
            if(original.state==State::Stored) {
                {StoreGuard guard;if(!store.Retain(original.slot))return false;entry.slot=original.slot;}
                if(failure_==2)return false;
            } else if(original.state==State::Recovery) {
                auto physical=PmmAllocPage(MemTag::User,childPid);if(!physical)return false;
                memcpy(reinterpret_cast<void*>(PhysToVirt(physical).raw()),
                    reinterpret_cast<const void*>(PhysToVirt(original.recovery).raw()),4096);
                entry.recovery=physical;++child->heldFrames;if(failure_==3)return false;
            } else if(original.state==State::Resident)++child->heldFrames;
            else if(original.state!=State::Zero && original.state!=State::Discarded)return false;
        }
    }
    return failure_!=4;
}
bool PageableMemoryFork::ValidateLocked() const {
    for(uint64_t n=0;n<count_;++n) {
        const auto& record=records_[n];auto* region=static_cast<Region*>(record.parent->object);
        if(region->pages!=record.pages)return false;
        for(uint64_t i=0;i<record.pages;++i) {
            const auto& current=region->entries[i];const auto& original=record.snapshot[i];
            if(current.state!=original.state || current.slot!=original.slot || current.recovery!=original.recovery)return false;
        }
    }
    return true;
}
MappingBacking* PageableMemoryFork::ChildBackingFor(MappingBacking* backing) const {
    auto* record=Find(backing);return record?record->child:nullptr;
}
PageableMemoryFork::~PageableMemoryFork() {
    for(uint64_t n=0;n<count_;++n) {
        auto& record=records_[n];
        for(uint64_t i=0;i<record.captured;++i)if(record.snapshot[i].recovery)PmmUnrefPage(record.snapshot[i].recovery);
        MappingBackingUnref(record.child);MappingBackingUnref(record.parent);
    }
    kfree(spans_);kfree(index_);kfree(snapshots_);kfree(records_);AddressSpaceUnref(source_);SchedulerKernelRegionLeave(actor_);
}
struct PageableMemoryRetirement::Page {
    MappingBacking* backing;
    Entry* entry;
    Entry original;
};
PageableMemoryRetirement::PageableMemoryRetirement(Process* actor):actor_(actor) {
    // Outlive the inner transaction's cancellation region: committed content
    // and backing pins must be released before fatal cancellation can unwind.
    SchedulerKernelRegionEnter(actor_);
}
uint64_t PageableMemoryRetirement::Count(const MappingRangeTable& table,uint64_t begin,uint64_t end) {
    uint64_t pages=0;
    for(auto* node=table.head;node && node->range.begin<end;node=node->next) {
        const auto& range=node->range;
        if(range.kind!=MappingKind::Pageable || range.end<=begin)continue;
        const uint64_t left=range.begin>begin?range.begin:begin,right=range.end<end?range.end:end;
        pages+=(right-left)/4096;
    }
    return pages;
}
bool PageableMemoryRetirement::Prepare(uint64_t pages) {
    if(pages_ || capacity_ || count_)KernelPanic("paging: repeated retirement preparation");
    if(!pages)return true;
    if(pages>~uint64_t{0}/sizeof(Page))return false;
    pages_=static_cast<Page*>(kmalloc(pages*sizeof(Page)));
    if(!pages_)return false;
    capacity_=pages;return true;
}
bool PageableMemoryRetirement::Capture(AddressSpaceState* space,uint64_t begin,uint64_t end) {
    if(count_ || committed_)KernelPanic("paging: repeated retirement capture");
    for(auto* node=space->ranges.head;node && node->range.begin<end;node=node->next) {
        const auto& range=node->range;
        if(range.kind!=MappingKind::Pageable || range.end<=begin)continue;
        if(!space->pagingAdmission.Exclusive() || !range.backing || range.backing->release!=ReleaseRegion)return false;
        auto* region=static_cast<Region*>(range.backing->object);
        const uint64_t left=range.begin>begin?range.begin:begin,right=range.end<end?range.end:end;
        for(uint64_t at=left;at<right;at+=4096) {
            const uint64_t index=(range.offset+at-range.begin)/4096;
            if(count_==capacity_ || index>=region->pages || region->entries[index].state==State::Evicting ||
               region->entries[index].state==State::Discarded)return false;
            auto& page=pages_[count_++];
            page={range.backing,&region->entries[index],region->entries[index]};
            MappingBackingRef(page.backing);
        }
    }
    return true;
}
void PageableMemoryRetirement::Commit(bool resetToZero) {
    if(committed_)KernelPanic("paging: repeated retirement commit");
    for(uint64_t i=0;i<count_;++i) {
        auto& page=pages_[i];auto* region=static_cast<Region*>(page.backing->object);
        if(page.original.state==State::Resident || page.original.state==State::Recovery) {
            if(!region->heldFrames)KernelPanic("paging: retirement held-frame underflow");
            --region->heldFrames;
        }
        *page.entry={0,{},resetToZero?State::Zero:State::Discarded};
    }
    committed_=true;
}
PageableMemoryRetirement::~PageableMemoryRetirement() {
    for(uint64_t i=0;i<count_;++i) {
        const auto& page=pages_[i];
        if(committed_) {
            ReleaseSlot(page.original.slot);
            if(page.original.recovery)PmmUnrefPage(page.original.recovery);
        }
        MappingBackingUnref(page.backing);
    }
    kfree(pages_);
    SchedulerKernelRegionLeave(actor_);
}
void PageableMemoryBootInit() {
    char name[32]{};uint32_t size=FwCfgReadFile("opt/page-store-device",name,sizeof(name));
    if(!size || size>=sizeof(name))return;name[size]=0;
    auto* dev=DeviceFind(name);
    if(!dev || dev->type!=DeviceType::Block || !dev->ops->read || !dev->ops->write)return;
    const uint64_t block=DeviceBlockSize(dev),count=DeviceBlockCount(dev);
    if(!block || count>~uint64_t{0}/block)return;
    alignas(4096) static uint8_t bytes[4096];
    if(Read(dev,0,bytes,4096)!=4096)return;
    PageStoreHeader h{};memcpy(&h,bytes,sizeof(h));
    for(unsigned i=sizeof(h);i<4096;++i)if(bytes[i])return;
    if(!PageStoreHeaderValid(h,block*count) || h.slots>65536)return;
    auto* records=static_cast<PageStoreSlot*>(kmalloc(h.slots*sizeof(PageStoreSlot)));
    if(!records)return;
    PhysicalAddress initial[FaultFrameReserve::Capacity]{};unsigned allocated=0;
    for(;allocated<FaultFrameReserve::Capacity;++allocated) {
        initial[allocated]=PmmAllocPage(MemTag::KernelData,KernelPid);
        if(!initial[allocated])break;
    }
    if(allocated!=FaultFrameReserve::Capacity || !store.Initialize(h,block*count,records,h.slots,{dev,Read,Write})) {
        for(unsigned i=0;i<allocated;++i)PmmUnrefPage(initial[i]);kfree(records);return;
    }
    for(auto frame:initial)if(!faultReserve.Push(frame))KernelPanic("paging: reserve initialization");
    KMutexInit(&storeLock);AttachRamCompression();ready=true;
    char refill[2]{};auto refillSize=FwCfgReadFile("opt/page-store-refill",refill,sizeof(refill));
    if(refillSize) {
        refillEnabled=refillSize==1 && refill[0]=='1';
        if(!(refillSize==1 && (refill[0]=='0' || refill[0]=='1')))
            SerialPuts("paging: invalid reserve-refill flag (worker disabled)\n");
    }

    char clock[2]{};auto clockSize=FwCfgReadFile("opt/page-store-executable-clock",clock,sizeof(clock));
    if(clockSize) {
        applicationClock=clockSize==1 && clock[0]=='1';
        if(!(clockSize==1 && (clock[0]=='0' || clock[0]=='1')))
            SerialPuts("paging: invalid executable clock flag (FIFO retained)\n");
        if(applicationClock)SerialPuts("paging: executable clock experiment enabled (acknowledged access-bit sweeps)\n");
    }
    char executable[256]{};auto executableSize=FwCfgReadFile("opt/page-store-executable",executable,sizeof(executable));
    if(executableSize) {
        uint64_t mib=64;char target[8]{};
        auto targetSize=FwCfgReadFile("opt/page-store-executable-resident-mib",target,sizeof(target));
        bool valid=targetSize<sizeof(target);
        if(targetSize) {
            mib=0;
            for(unsigned i=0;i<targetSize;++i) {
                if(target[i]<'0' || target[i]>'9'){valid=false;break;}
                mib=mib*10+unsigned(target[i]-'0');
            }
        }
        uint64_t headroom=32;char pressure[8]{};
        auto pressureSize=FwCfgReadFile("opt/page-store-executable-headroom-mib",pressure,sizeof(pressure));
        if(pressureSize) {
            if(pressureSize>=sizeof(pressure))valid=false;
            headroom=0;
            for(unsigned i=0;i<pressureSize && i<sizeof(pressure);++i) {
                if(pressure[i]<'0' || pressure[i]>'9'){valid=false;break;}
                headroom=headroom*10+unsigned(pressure[i]-'0');
            }
        }
        if(!valid || mib<1 || mib>256 || headroom>256 || !applicationPolicy.Configure(executable,executableSize,mib*256,headroom*256))
            SerialPuts("paging: invalid executable policy (ordinary mmap retained)\n");
        else SerialPrintf("paging: executable experiment path=%s resident_target_mib=%lu headroom_mib=%lu (single-task CLONE_VM restriction)\n",executable,mib,headroom);
    }
    SerialPrintf("paging experiment: backing=%s slots=%lu single-task admission\n",name,h.slots);
}
PageableBackingStatus PageableMemoryBackingStatus() {
    PageableBackingStatus status{};status.configured=ready;
    if(!ready || !KMutexTryLock(&storeLock))return status;
    const auto stats=store.Stats();
    status.usageValid=true;status.logicalCapacityBytes=stats.slots*4096;
    status.logicalUsedBytes=stats.used*4096;
    KMutexUnlock(&storeLock);return status;
}
void PageableMemoryStartReserveWorker() {
    if(!ready || !refillEnabled || __atomic_load_n(&refillStarted,__ATOMIC_ACQUIRE))return;
    auto* worker=KernelThreadCreate("page_reserve",ReserveRefillWorker,nullptr,1);
    if(!worker){SerialPuts("paging: reserve refill worker unavailable\n");return;}
    __atomic_store_n(&refillStarted,true,__ATOMIC_RELEASE);SchedulerAddProcess(worker);
}
int64_t PageableMemoryControl(Process* actor,uint64_t operation,uint64_t value) {
    if(!ready)return -95;
    if(!actor || !ProcessCapable(actor,nullptr,CapSysAdmin))return -1;
    if(operation==24)return AgeOne(actor,value);
    if(operation==22 && value<19)return __atomic_load_n(&applicationStats[value],__ATOMIC_RELAXED);
    if(operation==23 && value<512)return __atomic_load_n(&applicationBarriers[value],__ATOMIC_RELAXED);
    if(operation==20 && value<=5){StoreGuard guard;forkFailure=value;forkFailureSpace=value?actor->addressSpace->identity:0;return 0;}
    if(operation==21 && value<=2){StoreGuard guard;growthFailure=value;growthFailureSpace=value?actor->addressSpace->identity:0;return 0;}
    if(operation==0)return Map(actor,value);
    if(operation==1)return PageOut(actor,value);
    if(operation==2){StoreGuard guard;return store.Stats().stored;}
    if(operation==3)return PmmGetFreePageCount();
    if(operation==4 && value<=8){StoreGuard guard;injectedFailure=value;return 0;}
    if(operation==5){UserAccessScope lease(actor);if(!UserBufferReadable(value,4096))return -14;return PageOut(actor,value);}
    if(operation==6){KMutexLock(&actor->addressSpace->mutation);auto count=actor->addressSpace->userAccess.Count();KMutexUnlock(&actor->addressSpace->mutation);return count;}
    if(operation==7)return ReserveSize();
    if(operation==18) {
        if(value==0)return __atomic_load_n(&reserveRefilled,__ATOMIC_RELAXED);
        if(value==1)return __atomic_load_n(&reserveLowHeadroom,__ATOMIC_RELAXED);
        if(value==2)return __atomic_load_n(&reserveAllocationFailures,__ATOMIC_RELAXED);
        if(value==3)return __atomic_load_n(&refillStarted,__ATOMIC_ACQUIRE);
        return -22;
    }

    if(operation==8 && value<=32){auto flags=IrqSpinLockAcquire(&reserveLock);failOrdinaryAllocations=value;IrqSpinLockRelease(&reserveLock,flags);return 0;}
    if(operation==9 && value<=1) {
        KMutexLock(&actor->addressSpace->mutation);
        if(!actor->addressSpace->pagingAdmission.Exclusive()){KMutexUnlock(&actor->addressSpace->mutation);return -95;}
        actor->addressSpace->pagingReclaimEnabled=value!=0;
        KMutexUnlock(&actor->addressSpace->mutation);return 0;
    }
    if(operation==10)return ReclaimOne(actor,value&~uint64_t{4095});
    if(operation==11)return Map(actor,value,true);
    if(operation==12 && value<=65536) {
        KMutexLock(&actor->addressSpace->mutation);
        if(!actor->addressSpace->pagingAdmission.Exclusive()){KMutexUnlock(&actor->addressSpace->mutation);return -95;}
        actor->addressSpace->pagingResidentTarget=value;
        KMutexUnlock(&actor->addressSpace->mutation);return 0;
    }
    if(operation==14 && value<=15) {
        StoreGuard guard;auto stats=store.Stats();
        switch(value) {
            case 0:return ramCompression?ramCompression->pool.Capacity():0;
            case 1:return ramCompression?ramCompression->pool.Allocated():0;
            case 2:return ramCompression?ramCompression->pool.Payload():0;
            case 3:return stats.ramSlots;case 4:return stats.diskSlots;
            case 5:return stats.compressionFallbacks;
            case 6:return ramCompression?ramCompression->residentBytes:0;
            case 7:return stats.slots*sizeof(PageStoreSlot);
            case 8:return stats.spillSuccesses;case 9:return stats.spillFailures;
            case 10:return stats.decodeErrors;case 11:return stats.corruptReads;
            case 12:return stats.diskWrites;case 13:return stats.diskReads;
            case 14:return stats.ramWrites;case 15:return stats.ramReads;
        }
    }
    if(operation==15)return SpillStoredPage(actor,value);
    if(operation==16 && value<=2){auto s=FilePageCacheStatistics();return value==0?s.cached:value==1?s.scanned:s.reclaimed;}
    if(operation==17 && value<=1){FilePageCachePauseReclaim(value!=0);return 0;}
    if(operation==13 && value<=4) {
        auto* space=actor->addressSpace;uint64_t count=0;
        KMutexLock(&space->mutation);
        if(!value)count=HeldFrames(space);
        else for(auto* node=space->ranges.head;node;node=node->next) {
            const auto& range=node->range;
            if(range.kind!=MappingKind::Pageable || !range.backing || range.backing->release!=ReleaseRegion)continue;
            auto* region=static_cast<Region*>(range.backing->object);
            const uint64_t first=range.offset/4096,last=first+(range.end-range.begin)/4096;
            for(uint64_t i=first;i<last && i<region->pages;++i) {
                const auto state=region->entries[i].state;
                if((value==1 && state==State::Zero) || (value==2 && state==State::Stored) ||
                   (value==3 && state==State::Recovery) || (value==4 && state==State::Evicting))++count;
            }
        }
        KMutexUnlock(&space->mutation);return count;
    }
    return -22;
}
MappingFaultResult PageableMemoryFault(Process* actor,AddressSpaceState* space,uint64_t page,uint32_t access,bool allowReclaim) {
    if(!ready || !actor || !space || !space->pagingAdmission.Exclusive())return MappingFaultResult::Unhandled;
    FaultLifetime lifetime(actor,space);
    Backing backing;Frame recovery;uint64_t index=0,token=0;
    ReserveCheckout reserve;
    MappingTransaction op(actor,space);
    if(!op.LockFaultRange(page))return MappingFaultResult::OutOfMemory;
    MappingFaultSnapshot snapshot{};
    if(!op.CaptureRange(page,access,snapshot) || snapshot.range.kind!=MappingKind::Pageable ||
       !snapshot.range.backing || snapshot.range.backing->release!=ReleaseRegion)return MappingFaultResult::Unhandled;
    if(VmmReadLeaf(space->pageTable,VirtualAddress(page))&VMM_PRESENT)return MappingFaultResult::Resolved;
    backing.value=snapshot.range.backing;MappingBackingRef(backing.value);
    auto* region=static_cast<Region*>(backing.value->object);
    index=(snapshot.range.offset+page-snapshot.range.begin)/4096;
    if(index>=region->pages)return MappingFaultResult::Unhandled;
    auto& entry=region->entries[index];token=entry.slot;
    const auto initialState=entry.state;
    if(initialState==State::Recovery){recovery.physical=entry.recovery;PmmRefPage(recovery.physical);}
    else if(initialState!=State::Stored && initialState!=State::Zero)return MappingFaultResult::Unhandled;
    const uint64_t observedHeld=(space->pagingResidentTarget || space->pagingApplication)?HeldFrames(space):0;
    ApplicationObserveHeld(space,observedHeld);
    bool pressureAllowed=true;
    if(space->pagingApplication) {
        const bool previous=space->pagingPressureActive;
        pressureAllowed=applicationPolicy.ReclaimAllowed(space->pagingPressureActive,PmmGetFreePageCount());
        if(previous!=space->pagingPressureActive)ApplicationCount(space,space->pagingPressureActive?10:11);
    }
    const bool targetPressure=allowReclaim && actor->addressSpace==space &&
        space->pagingResidentTarget && observedHeld>=space->pagingResidentTarget;
    if(targetPressure && !pressureAllowed)ApplicationCount(space,12);
    const bool softPressure=targetPressure && pressureAllowed;
    op.Unlock();
    if(!op.Prepare(0,1))return MappingFaultResult::OutOfMemory;
    bool reclaimAttempted=softPressure && initialState!=State::Recovery;
    if(reclaimAttempted) {
        ApplicationCount(space,7);
        if(ReclaimOne(actor,page,true,&recovery.physical)<0)ApplicationCount(space,8);
    }
    PhysicalAddress physical=recovery.physical;
    if(!physical)physical=AllocateFaultFrame(space->allocationPid);
    if(!physical && reserve.Take(space->allocationPid)){physical=reserve.physical;PmmRefPage(physical);}
    if(!physical && !reclaimAttempted && allowReclaim && actor->addressSpace==space) {
        reclaimAttempted=true;ApplicationCount(space,7);
        if(ReclaimOne(actor,page,false,&recovery.physical)<0)ApplicationCount(space,8);
        physical=recovery.physical;
    }
    if(physical) {
        if(!op.StagePage(0,physical,true)){PmmUnrefPage(physical);recovery.physical={};return MappingFaultResult::OutOfMemory;}
        recovery.physical={};
    }
    if(reclaimAttempted) {
        // Even failed victim I/O may detach and restore a leaf, changing AS
        // generation. A full/no-victim scan may leave it unchanged. Recapture only the
        // SAME target mapping and stored entry; do not weaken final validation.
        MappingFaultSnapshot refreshed{};
        if(!op.LockFaultRange(page))return MappingFaultResult::OutOfMemory;
        if(!op.CaptureRange(page,access,refreshed) ||
           refreshed.range.identity!=snapshot.range.identity ||
           !Match(&refreshed.range,backing.value,page,index) ||
           entry.state!=initialState || entry.slot!=token)return MappingFaultResult::Unhandled;
        snapshot=refreshed;op.Unlock();
        if(!physical && reserve.Take(space->allocationPid)) {
            physical=reserve.physical;PmmRefPage(physical);
            if(!op.StagePage(0,physical,true)){PmmUnrefPage(physical);return MappingFaultResult::OutOfMemory;}
        }
    }
    // Reclaim failure belongs to resource acquisition, never to the incoming
    // page's data/checksum. PageOut preserves a failed victim's only good bytes.
    if(!physical)return MappingFaultResult::OutOfMemory;
    if(initialState==State::Zero)memset(reinterpret_cast<void*>(PhysToVirt(physical).raw()),0,4096);
    if(token) {
        int result;{StoreGuard guard;result=store.Load(token,reinterpret_cast<void*>(PhysToVirt(physical).raw()));}
        if(result)return MappingFaultResult::IoError;
    }
    bool rejectPublication=false;
    {StoreGuard guard;if(injectedFailure==7){rejectPublication=true;injectedFailure=0;}}
    if(!op.LockFaultRange(page))return MappingFaultResult::OutOfMemory;
    if(!op.ValidateRange(snapshot,page,access) || entry.slot!=token)return MappingFaultResult::Unhandled;
    if(rejectPublication)return MappingFaultResult::Unhandled;
    if(op.PublishFault(snapshot,0,VirtualAddress(page),access,false)!=MappingFaultCommit::Mapped)return MappingFaultResult::Unhandled;
    reserve.published=true;
    // Release backing-owned recovery/slot only after leaving the AS gate.
    recovery.physical=entry.recovery;entry.recovery={};entry.slot=0;entry.state=State::Resident;
    if(initialState!=State::Recovery)++region->heldFrames;
    ApplicationCount(space,initialState==State::Zero?2:initialState==State::Stored?3:4);
    space->pagingRecentFaults[space->pagingRecentNext++%8]=page;
    op.Unlock();ReleaseSlot(token);
    return MappingFaultResult::Resolved;
}
int PageableMemoryPrepareSyscall(Process* actor,uint64_t number) {
    if(!ready || !actor || !actor->addressSpace || !actor->addressSpace->pagingAdmission.Exclusive())return 0;
    if(number==0 || number==1 || number==2 || number==3 || number==5 || number==257 || number==228 || number==0xB40 || number==60 || number==231 || number==9 || number==11 || number==12 || number==89 || number==267 || number==57 || number==56 ||
       number==10 || number==25 || number==28 || number==62 || number==200 || number==234)return 0;
    // A sole task cannot mutate its mappings concurrently, but still take the
    // AS gate against diagnostic/cleanup users. Never retain a node after unlock.
    uint64_t cursor=0;
    for(;;) {
        uint64_t missing=0;uint32_t access=0;
        KMutexLock(&actor->addressSpace->mutation);
        for(auto* node=actor->addressSpace->ranges.head;node && !missing;node=node->next) {
            if(node->range.kind!=MappingKind::Pageable || node->range.end<=cursor || !node->range.protection)continue;
            uint64_t at=node->range.begin>cursor?node->range.begin:cursor;
            for(;at<node->range.end;at+=4096)
                if(!(VmmReadLeaf(actor->addressSpace->pageTable,VirtualAddress(at))&VMM_PRESENT)){
                    missing=at;access=node->range.protection&1?1:node->range.protection&2?2:4;break;
                }
        }
        KMutexUnlock(&actor->addressSpace->mutation);
        if(!missing){
            if(actor->addressSpace->pagingApplication) {
                KMutexLock(&actor->addressSpace->mutation);
                ApplicationObserveHeld(actor->addressSpace,HeldFrames(actor->addressSpace));
                KMutexUnlock(&actor->addressSpace->mutation);
            }
            return 0;
        }
        auto result=PageableMemoryFault(actor,actor->addressSpace,missing,access);
        if(result!=MappingFaultResult::Resolved)return result==MappingFaultResult::OutOfMemory?-12:-5;
        ApplicationCount(actor->addressSpace,6);
        if(actor->addressSpace->pagingApplication && number<512)
            __atomic_fetch_add(&applicationBarriers[number],1,__ATOMIC_RELAXED);
        cursor=missing+4096;
    }
}
}

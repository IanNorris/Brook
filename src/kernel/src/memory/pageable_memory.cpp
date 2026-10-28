#include "pageable_memory.h"
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
FaultFrameReserve faultReserve;
IrqSpinLock reserveLock;
unsigned failOrdinaryAllocations=0;
unsigned ReserveSize() {
    auto flags=IrqSpinLockAcquire(&reserveLock);auto size=faultReserve.Size();
    IrqSpinLockRelease(&reserveLock,flags);return size;
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
enum class State : uint8_t { Resident, Evicting, Stored, Recovery, Zero };
struct Entry { uint64_t slot;PhysicalAddress recovery;State state; };
struct Region { uint64_t pages,heldFrames;Entry entries[1]; };
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
int64_t PageOut(Process* actor,uint64_t page) {
    if(page&4095)return -22;
    auto* space=actor->addressSpace;
    if(!space->pagingAdmission.Exclusive())return -95;
    Slot slot;
    {StoreGuard guard;slot.token=store.Reserve();}
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
    } // full TLB ACK precedes both I/O and retirement of the old PTE ref
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
        --static_cast<Region*>(backing.value->object)->heldFrames;
        KMutexUnlock(&space->mutation);
        ReturnReserveFrame(frame.physical);frame.physical={};return 4096;
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
// AS mutation held; whole dedicated mappings cannot split or share backings.
uint64_t HeldFrames(AddressSpaceState* space) {
    uint64_t total=0;
    for(auto* node=space->ranges.head;node;node=node->next)
        if(node->range.kind==MappingKind::Pageable && node->range.backing && node->range.backing->release==ReleaseRegion)
            total+=static_cast<Region*>(node->range.backing->object)->heldFrames;
    return total;
}
// One finite traversal, in batches of at most 64 metadata/page visits. A sole
// admitted task cannot concurrently add ranges; never retain a range pointer
// across unlock/yield/I/O. At most one victim write is attempted per call.
int64_t ReclaimOne(Process* actor,uint64_t excluded,bool protectRecent=false) {
    auto* space=actor->addressSpace;
    uint64_t flags;__asm__ volatile("pushfq; popq %0":"=r"(flags));
    if(!(flags&512))return -16; // no sleeping/I/O from an interrupt-disabled caller
    KMutexLock(&space->mutation);
    if(!space->pagingAdmission.Exclusive() || !space->pagingReclaimEnabled) {
        KMutexUnlock(&space->mutation);return -95;
    }
    const uint64_t start=space->pagingReclaimCursor;
    KMutexUnlock(&space->mutation);
    uint64_t cursor=start;bool wrapped=false;
    for(;;) {
        uint64_t victim=0;bool done=false;
        KMutexLock(&space->mutation);
        for(unsigned visited=0;visited<64;++visited) {
            const auto* range=MappingRangeAtOrAfter(space->ranges,cursor);
            if(!range) {
                if(!start || wrapped){done=true;break;}
                cursor=0;wrapped=true;continue;
            }
            const uint64_t page=range->begin>cursor?range->begin:cursor;
            if(wrapped && page>=start){done=true;break;}
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
            victim=page;break;
        }
        KMutexUnlock(&space->mutation);
        if(victim)return PageOut(actor,victim); // authoritative admission recheck
        if(done)return -12;
        SchedulerYield();__asm__ volatile("sti":::"memory");
    }
}

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
    SerialPrintf("paging experiment: backing=%s slots=%lu single-task explicit only\n",name,h.slots);
}
int64_t PageableMemoryControl(Process* actor,uint64_t operation,uint64_t value) {
    if(!ready)return -95;
    if(!actor || !ProcessCapable(actor,nullptr,CapSysAdmin))return -1;
    if(operation==0)return Map(actor,value);
    if(operation==1)return PageOut(actor,value);
    if(operation==2){StoreGuard guard;return store.Stats().stored;}
    if(operation==3)return PmmGetFreePageCount();
    if(operation==4 && value<=7){StoreGuard guard;injectedFailure=value;return 0;}
    if(operation==5){UserAccessScope lease(actor);if(!UserBufferReadable(value,4096))return -14;return PageOut(actor,value);}
    if(operation==6){KMutexLock(&actor->addressSpace->mutation);auto count=actor->addressSpace->userAccess.Count();KMutexUnlock(&actor->addressSpace->mutation);return count;}
    if(operation==7)return ReserveSize();
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
            for(uint64_t i=0;i<region->pages;++i) {
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
    const bool softPressure=allowReclaim && actor->addressSpace==space &&
        space->pagingResidentTarget && HeldFrames(space)>=space->pagingResidentTarget;
    op.Unlock();
    if(!op.Prepare(0,1))return MappingFaultResult::OutOfMemory;
    bool reclaimAttempted=softPressure && initialState!=State::Recovery;
    bool reclaimed=reclaimAttempted && ReclaimOne(actor,page,true)==4096;
    PhysicalAddress physical=recovery.physical;
    if(!physical && reclaimed && reserve.Take(space->allocationPid)){physical=reserve.physical;PmmRefPage(physical);}
    if(!physical)physical=AllocateFaultFrame(space->allocationPid);
    if(!physical && reserve.Take(space->allocationPid)){physical=reserve.physical;PmmRefPage(physical);}
    if(!physical && !reclaimAttempted && allowReclaim && actor->addressSpace==space) {
        reclaimAttempted=true;reclaimed=ReclaimOne(actor,page)==4096;
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
    space->pagingRecentFaults[space->pagingRecentNext++%8]=page;
    op.Unlock();ReleaseSlot(token);
    return MappingFaultResult::Resolved;
}
int PageableMemoryPrepareSyscall(Process* actor,uint64_t number) {
    if(!ready || !actor || !actor->addressSpace || !actor->addressSpace->pagingAdmission.Exclusive())return 0;
    if(number==0 || number==1 || number==228 || number==0xB40 || number==60 || number==231 || number==11 || number==57 || number==56 ||
       number==10 || number==25 || number==28 || number==62 || number==200 || number==234)return 0;
    // A sole task cannot mutate its mappings concurrently, but still take the
    // AS gate against diagnostic/cleanup users. Never retain a node after unlock.
    uint64_t cursor=0;
    for(;;) {
        uint64_t missing=0;
        KMutexLock(&actor->addressSpace->mutation);
        for(auto* node=actor->addressSpace->ranges.head;node && !missing;node=node->next) {
            if(node->range.kind!=MappingKind::Pageable || node->range.end<=cursor)continue;
            uint64_t at=node->range.begin>cursor?node->range.begin:cursor;
            for(;at<node->range.end;at+=4096)
                if(!(VmmReadLeaf(actor->addressSpace->pageTable,VirtualAddress(at))&VMM_PRESENT)){missing=at;break;}
        }
        KMutexUnlock(&actor->addressSpace->mutation);
        if(!missing)return 0;
        auto result=PageableMemoryFault(actor,actor->addressSpace,missing,1);
        if(result!=MappingFaultResult::Resolved)return result==MappingFaultResult::OutOfMemory?-12:-5;
        cursor=missing+4096;
    }
}
}

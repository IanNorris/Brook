#include "pageable_memory.h"
#include "page_store.h"
#include "user_mapping.h"
#include "physical_memory.h"
#include "device.h"
#include "fw_cfg.h"
#include "heap.h"
#include "process.h"
#include "process_user_namespace.h"
#include "serial.h"
#include "string.h"
#include "sync/kmutex.h"

namespace brook {
namespace {
PageStore store;
KMutex storeLock;
bool ready=false;
// Root-only one-shot failure controls for the explicitly boot-enabled test.
unsigned injectedFailure=0;
enum class State : uint8_t { Resident, Evicting, OnDisk, Recovery };
struct Entry { uint64_t slot;PhysicalAddress recovery;State state; };
struct Region { uint64_t pages;Entry entries[1]; };
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
int64_t Map(Process* actor,uint64_t length) {
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
    Backing backing{MappingBackingCreate(region,ReleaseRegion)};
    if(!backing.value){kfree(region);return -12;}
    UserMappingRequest request{};
    request.range={0,length,0,0,backing.value,3,3,MappingKind::Pageable,MappingSharing::Private,MappingLeafOwnership::Managed};
    request.length=length;request.preparedPages=pages;request.prepare=Prepare;
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
        entry.state=State::OnDisk;
        KMutexUnlock(&space->mutation);return 4096;
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
    if(!store.Initialize(h,block*count,records,h.slots,{dev,Read,Write})){kfree(records);return;}
    KMutexInit(&storeLock);ready=true;
    SerialPrintf("paging experiment: backing=%s slots=%lu single-task explicit only\n",name,h.slots);
}
int64_t PageableMemoryControl(Process* actor,uint64_t operation,uint64_t value) {
    if(!ready)return -95;
    if(!actor || !ProcessCapable(actor,nullptr,CapSysAdmin))return -1;
    if(operation==0)return Map(actor,value);
    if(operation==1)return PageOut(actor,value);
    if(operation==2){StoreGuard guard;return store.Stats().stored;}
    if(operation==3)return PmmGetFreePageCount();
    if(operation==4 && value<=6){StoreGuard guard;injectedFailure=value;return 0;}
    return -22;
}
MappingFaultResult PageableMemoryFault(Process* actor,AddressSpaceState* space,uint64_t page,uint32_t access) {
    if(!ready || !actor || !space->pagingAdmission.Exclusive())return MappingFaultResult::Unhandled;
    Backing backing;Frame recovery;uint64_t index=0,token=0;
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
    if(entry.state==State::Recovery){recovery.physical=entry.recovery;PmmRefPage(recovery.physical);}
    else if(entry.state!=State::OnDisk)return MappingFaultResult::Unhandled;
    op.Unlock();
    if(!op.Prepare(0,1))return MappingFaultResult::OutOfMemory;
    auto physical=recovery.physical?recovery.physical:PmmAllocPage(MemTag::User,space->allocationPid);
    if(!physical)return MappingFaultResult::OutOfMemory;
    op.StagePage(0,physical,true);recovery.physical={};
    if(token) {
        int result;{StoreGuard guard;result=store.Load(token,reinterpret_cast<void*>(PhysToVirt(physical).raw()));}
        if(result)return MappingFaultResult::IoError;
    }
    if(!op.LockFaultRange(page))return MappingFaultResult::OutOfMemory;
    if(!op.ValidateRange(snapshot,page,access) || entry.slot!=token)return MappingFaultResult::Unhandled;
    if(op.PublishFault(snapshot,0,VirtualAddress(page),access,false)!=MappingFaultCommit::Mapped)return MappingFaultResult::Unhandled;
    // Release backing-owned recovery/slot only after leaving the AS gate.
    recovery.physical=entry.recovery;entry.recovery={};entry.slot=0;entry.state=State::Resident;
    op.Unlock();ReleaseSlot(token);
    return MappingFaultResult::Resolved;
}
int PageableMemoryPrepareSyscall(Process* actor,uint64_t number) {
    if(!ready || !actor || !actor->addressSpace || !actor->addressSpace->pagingAdmission.Exclusive())return 0;
    if(number==0xB40 || number==60 || number==231 || number==11 || number==57 || number==56 ||
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

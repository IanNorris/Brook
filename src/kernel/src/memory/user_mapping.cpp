#include "user_mapping.h"
#include "physical_memory.h"
#include "process.h"
#include "panic.h"
#include "string.h"
#include "heap.h"
#include "serial.h"
#include "file_object.h"
#include "vfs.h"
#include "mapping_access_timing.h"
#include "exec_limits.h"

namespace brook {
// Source metadata and leaves are stable under AS.mutation. Destination is not
// published to the scheduler until the operation's parent shootdown completes.
static bool CloneLeaves(MappingTransaction& operation, AddressSpaceState* destination, uint16_t pid) {
    auto* source = operation.Space();
    constexpr uint64_t physicalMask = 0x000FFFFFFFFFF000ULL;
    auto* pml4 = reinterpret_cast<uint64_t*>(PhysToVirt(source->pageTable.pml4).raw());
    auto* range = source->ranges.head;
    for (uint64_t i4=0; i4<256; ++i4) {
        if (!(pml4[i4]&VMM_PRESENT)) continue;
        auto* pdpt = reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(pml4[i4]&physicalMask)).raw());
        for (uint64_t i3=0; i3<512; ++i3) {
            if (!(pdpt[i3]&VMM_PRESENT)) continue;
            if (pdpt[i3]&(1ULL<<7)) return false;
            auto* pd = reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(pdpt[i3]&physicalMask)).raw());
            for (uint64_t i2=0; i2<512; ++i2) {
                if (!(pd[i2]&VMM_PRESENT)) continue;
                if (pd[i2]&(1ULL<<7)) return false;
                auto* pt = reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(pd[i2]&physicalMask)).raw());
                for (uint64_t i1=0; i1<512; ++i1) {
                    uint64_t raw = pt[i1];
                    if (!VmmLeafBacked(raw)) continue;
                    uint64_t address=(i4<<39)|(i3<<30)|(i2<<21)|(i1<<12);
                    while (range && range->range.end<=address) range=range->next;
                    // Every populated user leaf must have authoritative metadata.
                    if (!range || range->range.begin>address) return false;
                    const auto& mapping=range->range;
                    bool managed=mapping.leafOwnership==MappingLeafOwnership::Managed;
                    bool cow=managed && mapping.sharing==MappingSharing::Private &&
                        (raw&(VMM_WRITABLE|PTE_COW_BIT));
                    uint64_t childFlags=raw&(VMM_USER|VMM_WRITABLE|VMM_NO_EXEC|PTE_COW_BIT|VMM_LEAF_CACHE_MASK);
                    if (cow) childFlags=(childFlags&~VMM_WRITABLE)|PTE_COW_BIT;
                    else if (mapping.sharing==MappingSharing::Shared) childFlags&=~PTE_COW_BIT;
                    PhysicalAddress physical(raw&physicalMask);
                    // Pin before publication. The locked CoW commit supplies its
                    // own child reference, replacing this anticipatory pin.
                    if (managed) PmmRefPage(physical);
                    auto result=VmmTryMapPage(destination->pageTable,VirtualAddress(address),physical,
                        childFlags,managed?MemTag::User:MemTag::Device,pid,(raw&VMM_PRESENT)!=0);
                    if (result!=VmmMapResult::Mapped) {
                        if (managed) PmmUnrefPage(physical);
                        return false;
                    }
                    if (cow) {
                        operation.RequireShootdown();
                        auto* childTable=reinterpret_cast<uint64_t*>(PhysToVirt(destination->pageTable.pml4).raw());
                        const uint64_t indices[]={i4,i3,i2};
                        for (uint64_t index:indices)
                            childTable=reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(childTable[index]&physicalMask)).raw());
                        auto* childLeaf=&childTable[i1];
                        VmmForkCommitCowShare(&pt[i1],childLeaf,*childLeaf,physical);
                        PmmUnrefPage(physical);
                    }
                }
            }
        }
    }
    return true;
}
bool UserMappingClone(Process* actor, AddressSpaceState* source, AddressSpaceState* destination, uint16_t pid) {
    if (!actor || !source || !destination || source->kernel || destination->kernel ||
        source==destination || destination->ranges.head) return false;
    for (;;) {
        MappingTransaction operation(actor,source);
        if(!operation.Lock())return false;
        if(source->pagingAdmission.Exclusive())return false;
        uint64_t count=0, generation=source->generation;
        for (auto* node=source->ranges.head; node; node=node->next) ++count;
        operation.Unlock();
        MappingRangePlan plan;
        if (!plan.Prepare(count)) return false;
        if(!operation.Lock())return false;
        if(source->pagingAdmission.Exclusive())return false;
        if (source->generation!=generation) { operation.Unlock(); continue; }
        for (auto* node=source->ranges.head; node; node=node->next)
            if (!plan.Insert(destination->ranges,node->range))
                KernelPanic("mapping: preallocated fork metadata failed");
        destination->programBreak=source->programBreak;
        destination->programBreakLow=source->programBreakLow;
        destination->programBreakHigh=source->programBreakHigh;
        destination->mmapNext=source->mmapNext;
        destination->argumentStart=source->argumentStart;
        destination->argumentEnd=source->argumentEnd;
        destination->membarrierPrivateRegistered=__atomic_load_n(
            &source->membarrierPrivateRegistered,__ATOMIC_ACQUIRE);
        bool copied=CloneLeaves(operation,destination,pid);
        operation.Unlock();
        return copied;
    }
}
uint32_t UserMappingReadArguments(AddressSpaceState* space,uint64_t offset,void* buffer,uint32_t capacity) {
    if (!space || space->kernel || !buffer || !capacity) return 0;
    KMutexLock(&space->mutation);
    uint32_t copied=0;
    const uint64_t begin=space->argumentStart,end=space->argumentEnd;
    if (begin>=VIRTUAL_NULL_GUARD && end>=begin && end<0x0000800000000000ULL &&
        end-begin<=EXEC_ARG_BYTES && offset<end-begin) {
        // Linux probes the final argument byte before reading the vector.
        // A PROT_NONE argument tail is EOF, not forced access to hidden data.
        const auto* tail=MappingRangeFind(space->ranges,end-1);
        const uint64_t last=VmmReadLeaf(space->pageTable,VirtualAddress(end-1));
        if (!tail || tail->kind==MappingKind::Device || !(tail->protection&1) || !(last&VMM_PRESENT) || !(last&VMM_USER)) {
            KMutexUnlock(&space->mutation);return 0;
        }
        uint64_t address=begin+offset,remaining=end-address;
        if (remaining>capacity) remaining=capacity;
        while (remaining) {
            const uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(address));
            const auto* range=MappingRangeFind(space->ranges,address);
            if (!range || range->kind==MappingKind::Device || !(range->protection&1) || !(leaf&VMM_PRESENT) || !(leaf&VMM_USER)) break;
            uint64_t chunk=4096-(address&4095);if (chunk>remaining) chunk=remaining;
            const auto* source=reinterpret_cast<const uint8_t*>(PhysToVirt(VmmLeafPhysical(leaf)).raw())+(address&4095);
            memcpy(static_cast<uint8_t*>(buffer)+copied,source,chunk);
            copied+=chunk;address+=chunk;remaining-=chunk;
        }
    }
    KMutexUnlock(&space->mutation);
    return copied;
}
struct BufferFrame { PhysicalAddress physical; uint64_t retired; };
struct BufferMapping { KMutex retirementLock;uint64_t pages;BufferFrame frames[1]; };
static void ReleaseBufferMapping(void* object) {
    auto* buffer=static_cast<BufferMapping*>(object);
    for (uint64_t i=0;i<buffer->pages;++i) PmmUnrefPage(buffer->frames[i].physical);
    kfree(buffer);
}
MappingBacking* UserMappingBufferSnapshot(uint64_t kernelAddress,uint64_t length) {
    if (!kernelAddress || (kernelAddress&4095) || !length || (length&4095) ||
        length>~uint64_t{0}-kernelAddress) return nullptr;
    uint64_t pages=length/4096;
    if (pages>(~uint64_t{0}-sizeof(BufferMapping))/sizeof(BufferFrame)) return nullptr;
    auto* buffer=static_cast<BufferMapping*>(kmalloc(sizeof(BufferMapping)+(pages-1)*sizeof(BufferFrame)));
    if (!buffer) return nullptr;
    KMutexInit(&buffer->retirementLock);buffer->pages=0;
    for (uint64_t i=0;i<pages;++i) {
        auto physical=VmmVirtToPhys(KernelPageTable,VirtualAddress(kernelAddress+i*4096));
        if (!physical || !PmmRefPageIfAlive(physical)) { ReleaseBufferMapping(buffer);return nullptr; }
        buffer->frames[buffer->pages++]={physical,0};
    }
    auto* descriptor=MappingBackingCreate(buffer,ReleaseBufferMapping);
    if (!descriptor) ReleaseBufferMapping(buffer);
    return descriptor;
}
static int PrepareBufferAlias(MappingTransaction& operation,const UserMappingRequest& request,void*) {
    auto* buffer=static_cast<BufferMapping*>(request.range.backing->object);
    for (uint64_t i=0;i<request.preparedPages;++i) {
        auto physical=buffer->frames[request.range.offset/4096+i].physical;PmmRefPage(physical);
        if (!operation.StagePage(i,physical,true)) { PmmUnrefPage(physical);return -12; }
    }
    return 0;
}
bool UserMappingConfigureBuffer(UserMappingRequest& request,MappingBacking* descriptor) {
    if (!descriptor || descriptor->release!=ReleaseBufferMapping || !request.length ||
        (request.length&4095) || (request.range.offset&4095) || (request.range.protection&~3u)) return false;
    auto* buffer=static_cast<BufferMapping*>(descriptor->object);
    uint64_t bytes=buffer->pages*4096;
    if (request.range.offset>=bytes || request.length>bytes-request.range.offset) return false;
    request.range.backing=descriptor;request.range.kind=MappingKind::Device;
    request.range.sharing=MappingSharing::Shared;request.range.leafOwnership=MappingLeafOwnership::Managed;
    request.range.maximumProtection&=3u;
    request.preparedPages=request.length/4096;request.prepare=PrepareBufferAlias;
    return true;
}
void UserFramebufferPublish(Process* process,MappingBacking* descriptor,uint32_t* pixels,uint32_t bytes,
                             uint32_t width,uint32_t height,uint32_t stride) {
    if (descriptor && descriptor->release!=ReleaseBufferMapping) KernelPanic("mapping: invalid framebuffer descriptor");
    if (descriptor) {
        auto* buffer=static_cast<BufferMapping*>(descriptor->object);
        if (!pixels || !bytes || !width || !height || stride<width ||
            static_cast<uint64_t>(stride)*4>bytes/height || bytes>buffer->pages*4096)
            KernelPanic("mapping: invalid framebuffer geometry");
    } else if (pixels || bytes || width || height || stride) KernelPanic("mapping: unretained framebuffer source");
    MappingBackingRef(descriptor);
    KMutexLock(&process->framebufferLock);
    auto* previous=process->fbBacking;process->fbBacking=descriptor;
    process->fbVirtual=pixels;process->fbVirtualSize=bytes;
    process->fbVfbWidth=width;process->fbVfbHeight=height;process->fbVfbStride=stride;process->fbDirty=1;
    KMutexUnlock(&process->framebufferLock);MappingBackingUnref(previous);
}
int64_t UserMappingBufferAlias(Process* actor,AddressSpaceState* space,MappingBacking* descriptor,uint32_t protection) {
    if (!descriptor || descriptor->release!=ReleaseBufferMapping || !protection || (protection&~3u)) return -22;
    auto* buffer=static_cast<BufferMapping*>(descriptor->object);
    UserMappingRequest request{};
    request.length=buffer->pages*4096;request.preparedPages=buffer->pages;request.prepare=PrepareBufferAlias;
    request.range={0,request.length,0,0,descriptor,protection,protection,MappingKind::Device,
                   MappingSharing::Shared,MappingLeafOwnership::Managed};
    if (!UserMappingConfigureBuffer(request,descriptor)) return -22;
    return UserMappingCreate(actor,space,request);
}
struct BufferRetirementGuard {
    BufferMapping* buffer;
    explicit BufferRetirementGuard(BufferMapping* value):buffer(value) { KMutexLock(&buffer->retirementLock); }
    ~BufferRetirementGuard() {
        // Constructed before the mapping transaction: its destructor/ACK always
        // runs first, including early returns after a partial committed removal.
        for (uint64_t i=0;i<buffer->pages;++i) {
            auto& frame=buffer->frames[i];
            while (frame.retired) { --frame.retired;PmmUnrefPage(frame.physical); }
        }
        KMutexUnlock(&buffer->retirementLock);
    }
};
struct BufferRetirementContext { AddressSpaceState* space;BufferMapping* buffer;MappingBacking* descriptor; };
static void RetireBufferLeaf(VirtualAddress address,uint64_t raw,void* opaque) {
    auto& context=*static_cast<BufferRetirementContext*>(opaque);
    const auto* range=MappingRangeFind(context.space->ranges,address.raw());
    if (!range || range->backing!=context.descriptor) KernelPanic("mapping: buffer retirement range changed");
    uint64_t index=(range->offset+address.raw()-range->begin)/4096;
    if (index>=context.buffer->pages || context.buffer->frames[index].physical!=VmmLeafPhysical(raw))
        KernelPanic("mapping: buffer retirement frame changed");
    auto& count=context.buffer->frames[index].retired;
    if (count==~uint64_t{0}) KernelPanic("mapping: buffer retirement count exhausted");
    ++count;
}
static int64_t RemoveBufferMappings(Process* actor,AddressSpaceState* space,MappingBacking* descriptor) {
    auto* buffer=static_cast<BufferMapping*>(descriptor->object);
    BufferRetirementGuard retirement(buffer);
    MappingTransaction operation(actor,space,nullptr,descriptor);
    // Exact whole-node removals use no split nodes. Per-frame retirement counters
    // were reserved with the immutable source snapshot, so exit needs no heap.
    if (!operation.PrepareRanges(0)) return -12;
    if(!operation.Lock())return -12;
    for (auto* node=space->ranges.head;node;node=node->next) if (node->range.backing==descriptor) {
        uint64_t count=0;
        if (!operation.RangeLeafCount(node->range.begin,node->range.end,count) ||
            !operation.CanReplaceRange(node->range.begin,node->range.end)) return -22;
    }
    BufferRetirementContext context{space,buffer,descriptor};
    for (auto* node=space->ranges.head;node;) {
        auto* next=node->next;
        if (node->range.backing==descriptor &&
            !operation.RemoveMappingWithRetirer(node->range.begin,node->range.end,RetireBufferLeaf,&context))
            KernelPanic("mapping: preflighted buffer removal failed");
        node=next;
    }
    return 0;
}
int64_t UserMappingRemoveBacking(Process* actor,AddressSpaceState* space,MappingBacking* backing) {
    if (!actor || !space || !backing) return -22;
    if (backing->release==ReleaseBufferMapping) return RemoveBufferMappings(actor,space,backing);
    for (;;) {
        MappingTransaction operation(actor,space);if(!operation.Lock())return -12;
        uint64_t count=0,generation=space->generation;
        for (auto* node=space->ranges.head;node;node=node->next) if (node->range.backing==backing) {
            uint64_t leaves=0;
            if (!operation.RangeLeafCount(node->range.begin,node->range.end,leaves)) return -22;
            if (leaves>~uint64_t{0}-count) return -12;
            count+=leaves;
        }
        operation.Unlock();
        if (!operation.Prepare(count,0) || !operation.PrepareRanges(0)) return -12;
        if(!operation.Lock())return -12;if (space->generation!=generation) continue;
        uint64_t required=0;
        for (auto* node=space->ranges.head;node;node=node->next) if (node->range.backing==backing) {
            uint64_t leaves=0;
            if (!operation.RangeLeafCount(node->range.begin,node->range.end,leaves) ||
                !operation.CanReplaceRange(node->range.begin,node->range.end)) return -22;
            if (leaves>count-required) { required=count+1;break; }
            required+=leaves;
        }
        if (required>count) continue;
        for (auto* node=space->ranges.head;node;) {
            auto* next=node->next;
            if (node->range.backing==backing && !operation.ReplaceMapping(node->range.begin,node->range.end))
                KernelPanic("mapping: preflighted backing removal failed");
            node=next;
        }
        return 0;
    }
}
struct StackPreparation { const uint8_t* arguments; uint64_t length,offset; };
static int PrepareStack(MappingTransaction& operation,const UserMappingRequest& request,void* opaque) {
    if (!operation.PrepareZeroed(MemTag::User,operation.Space()->allocationPid)) return -12;
    auto& preparation=*static_cast<StackPreparation*>(opaque);
    uint64_t copied=0;
    while (copied<preparation.length) {
        uint64_t position=preparation.offset+copied,offset=position&4095;
        uint64_t count=4096-offset;
        if (count>preparation.length-copied) count=preparation.length-copied;
        if (position/4096>=request.preparedPages) return -22;
        auto physical=operation.PreparedPage(position/4096);
        memcpy(reinterpret_cast<void*>(PhysToVirt(physical).raw()+offset),preparation.arguments+copied,count);
        copied+=count;
    }
    return 0;
}
int64_t UserMappingStack(Process* actor,AddressSpaceState* space,uint64_t size,
                          const void* arguments,uint64_t argumentLength,uint64_t* argumentAddress) {
    if (!size || (size&4095) || size>(1u<<20) || argumentLength>size/2 ||
        (argumentLength && !arguments) || !argumentAddress) return -22;
    StackPreparation preparation{static_cast<const uint8_t*>(arguments),argumentLength,(size-argumentLength)&~uint64_t{15}};
    UserMappingRequest request{};
    request.range={0,size,0,0,nullptr,3,3,MappingKind::Stack,MappingSharing::Private,MappingLeafOwnership::Managed};
    request.length=size;request.preparedPages=size/4096;request.prepare=PrepareStack;request.context=&preparation;
    int64_t base=UserMappingCreate(actor,space,request);
    if (base>=0) *argumentAddress=argumentLength?static_cast<uint64_t>(base)+preparation.offset:0;
    return base;
}
static bool UserInterval(uint64_t address, uint64_t length, uint64_t& end) {
    constexpr uint64_t limit = 0x0000800000000000ULL;
    if ((address & 4095) || !length || length > limit || address >= limit) return false;
    length = (length + 4095) & ~uint64_t{4095};
    if (length > limit-address) return false;
    end = address+length; return true;
}
static uint64_t PickMappingAddress(AddressSpaceState* space, const UserMappingRequest& request) {
    if (request.fixed) return request.hint;
    const uint64_t limit=request.low32?0x80000000ULL:USER_MMAP_END;
    if(request.length>limit-USER_MMAP_BASE)return 0;
    uint64_t start = request.hint >= USER_MMAP_BASE && request.hint < limit ?
        request.hint & ~uint64_t{4095} : request.low32?USER_MMAP_BASE:space->mmapNext;
    if (start < USER_MMAP_BASE || start >= limit) start = USER_MMAP_BASE;
    auto found = MappingRangeFindGap(space->ranges,start,request.length,USER_MMAP_BASE,limit);
    return found ? found : MappingRangeFindGap(space->ranges,USER_MMAP_BASE,request.length,USER_MMAP_BASE,limit);
}
int64_t UserMappingCreate(Process* actor, AddressSpaceState* space, const UserMappingRequest& request) {
    // Declared before every transaction so errors are printed only after AS /
    // file gates and transaction retirement have unwound. No successful calls
    // emit diagnostics; no backing pointers are inspected by the destructor.
    struct FailureTrace {
        Process* actor;
        const UserMappingRequest& request;
        const char* stage = nullptr;
        int64_t error = 0;
        uint64_t retired = 0, attempts = 0;
        int64_t Fail(const char* where,int64_t result) { stage=where;error=result;return result; }
        ~FailureTrace() {
            if (stage && actor && actor->straceEnabled && actor->straceErrorsOnly)
                SerialPrintf("[mapping-create-error:%u] stage=%s error=%ld hint=0x%lx length=%lu fixed=%u kind=%u prepared=%lu retired=%lu attempts=%lu\n",
                    actor->pid,stage,error,request.hint,request.length,unsigned(request.fixed),
                    unsigned(request.range.kind),request.preparedPages,retired,attempts);
        }
    } failure{actor,request};
    uint64_t checkedEnd;
    MappingRange checked = request.range;
    checked.begin = 0; checked.end = request.length;
    if (!actor || !space || (request.noReplace && !request.fixed) || (request.length & 4095) ||
        !UserInterval(request.fixed ? request.hint : 0,request.length,checkedEnd) ||
        (request.fixed && request.hint < VIRTUAL_NULL_GUARD) ||
        (request.range.protection & ~request.range.maximumProtection) ||
        (request.range.maximumProtection & ~7u) ||
        !MappingRangeValid(checked) ||
        (request.preparedPages && (request.preparedPages != request.length/4096 || !request.prepare))) return failure.Fail("request-validation",-22);
    if ((request.cacheFlags & ~VMM_LEAF_CACHE_MASK) ||
        (request.cacheFlags && (request.range.kind != MappingKind::Device ||
         request.range.leafOwnership != MappingLeafOwnership::Borrowed ||
         request.preparedPages != request.length / 4096))) return failure.Fail("cache-policy",-22);
    for (;;) {
        ++failure.attempts;
        MappingTransaction operation(actor,space,request.range.backing);
        PmmThrottleForDrain();
        if(!operation.Lock())return failure.Fail("gate-set-snapshot",-12);
        if(request.range.backing && request.range.backing->file && request.range.backing->file->mappingError)
            return failure.Fail("file-quarantine",request.range.backing->file->mappingError);
        const uint64_t address = PickMappingAddress(space,request);
        if (!address) return failure.Fail("address-gap",-12);
        const uint64_t end = address+request.length;
        if(request.fixed && !request.noReplace && !MappingRangeRemovalAllowed(space->ranges,address,end))
            return failure.Fail("pageable-partial-replacement",-95);
        if (request.noReplace &&
            MappingRangeFindGap(space->ranges,address,request.length,address,end) != address)
            return failure.Fail("exact-address-occupied",-17); // EEXIST, without retiring anything
        uint64_t retired = 0;
        if (request.fixed && !request.noReplace && !operation.RangeLeafCount(address,end,retired)) return failure.Fail("retirement-count",-22);
        failure.retired=retired;
        const uint64_t generation = space->generation;
        operation.Unlock();
        if (!operation.Prepare(retired,request.preparedPages)) return failure.Fail("page-record-storage",-12);
        if (!operation.PrepareRanges(3)) return failure.Fail("range-node-storage",-12);
        if (request.prepare) {
            int result = request.prepare(operation,request,request.context);
            if (result) return failure.Fail("backing-prepare",result);
        }
        for (uint64_t i=0;i<request.preparedPages;++i)
            if (!operation.PreparedPage(i)) return failure.Fail("prepared-page-missing",-12);
        if (request.preparedPages && !VmmPrepareRange(space->pageTable,address,end)) return failure.Fail("page-table-prepare",-12);
        if(!operation.Lock())return failure.Fail("gate-set-commit",-12);
        if(request.range.backing && request.range.backing->file && request.range.backing->file->mappingError)
            return failure.Fail("file-quarantine",request.range.backing->file->mappingError);
        if (space->generation != generation) continue;
        MappingRange range = request.range;
        range.begin = address; range.end = end; range.identity = 0;
        if (request.fixed && !request.noReplace) {
            if (!operation.CanReplaceRange(address,end,&range)) return failure.Fail("range-replace-preflight",-12);
            // A fault may have populated another leaf without changing topology
            // while preparation was unlocked. Capacity failure changes nothing.
            if (!operation.ReplaceMapping(address,end,&range)) continue;
        } else if (!operation.InsertRange(range)) return failure.Fail("range-insert",-12);
        uint64_t flags = VMM_USER | request.cacheFlags;
        if (!(range.protection & 4)) flags |= VMM_NO_EXEC;
        if (range.protection & 2) flags |= VMM_WRITABLE;
        MemTag tag = range.kind == MappingKind::Device && range.leafOwnership == MappingLeafOwnership::Borrowed ?
            MemTag::Device : MemTag::User;
        for (uint64_t i=0;i<request.preparedPages;++i)
            if (operation.Publish(i,VirtualAddress(address+i*4096),flags,tag,space->allocationPid,
                                  range.protection != 0) != VmmMapResult::Mapped)
                KernelPanic("mapping: preflighted page publication failed");
        if (!request.fixed && !request.low32 && end > space->mmapNext) space->mmapNext = end;
        return static_cast<int64_t>(address);
    }
}
int64_t UserMappingRemove(Process* actor, AddressSpaceState* space, uint64_t address, uint64_t length) {
    uint64_t end;
    if (!actor || !space || !UserInterval(address,length,end)) return -22;
    for (;;) {
        MappingTransaction operation(actor,space);
        PmmThrottleForDrain(); if(!operation.Lock())return -12;
        if(!MappingRangeRemovalAllowed(space->ranges,address,end))return -95;
        uint64_t count = 0;
        if (!operation.RangeLeafCount(address,end,count)) return -22;
        uint64_t generation = space->generation;
        operation.Unlock();
        if (!operation.Prepare(count,0) || !operation.PrepareRanges(2)) return -12;
        if(!operation.Lock())return -12;
        if (space->generation != generation) continue;
        if (!operation.CanReplaceRange(address,end)) return -12;
        if (!operation.ReplaceMapping(address,end)) continue;
        return 0;
    }
}
int64_t UserMappingProtect(Process* actor, AddressSpaceState* space, uint64_t address, uint64_t length, uint32_t protection) {
    uint64_t end;
    if (!actor || !space || (address & 4095) || (protection & ~7u)) return -22;
    if (!length) return 0;
    if (!UserInterval(address,length,end)) return -22;
    MappingTransaction operation(actor,space);
    if (!operation.PrepareRanges(2)) return -12;
    if(!operation.Lock())return -12;
    for (uint64_t at=address;at<end;) {
        const auto* range = MappingRangeFind(space->ranges,at);
        if (!range) return -12;
        if(range->kind==MappingKind::Pageable)return -95;
        if (protection & ~range->maximumProtection) return -13;
        if (protection && range->backing && range->backing->file && range->backing->file->mappingError)
            return range->backing->file->mappingError;
        at = range->end < end ? range->end : end;
    }
    return operation.ProtectMapping(address,end,protection) ? 0 : -12;
}
int64_t UserMappingSync(Process* actor,AddressSpaceState* space,uint64_t address,uint64_t length,uint64_t flags) {
    if(!actor || !space || (address&4095) || (flags&~7ULL) || ((flags&1)&&(flags&4)))return -22;
    if(!length)return 0;
    uint64_t end;
    if(!UserInterval(address,length,end))return -12;
    int firstError=0;
    for(uint64_t at=address;at<end;) {
        MappingTransaction operation(actor,space);
        if(!operation.Lock())return firstError?firstError:-12;
        const auto* range=MappingRangeFind(space->ranges,at);
        if(!range) {
            if(!firstError)firstError=-12;
            uint64_t next=end;
            for(auto* node=space->ranges.head;node;node=node->next)
                if(node->range.begin>at){if(node->range.begin<next)next=node->range.begin;break;}
            at=next;continue;
        }
        const uint64_t right=range->end<end?range->end:end;
        if(range->kind!=MappingKind::File || range->sharing!=MappingSharing::Shared) {at=right;continue;}
        MappingFaultSnapshot snapshot{};
        if(!operation.CaptureMapping(at,snapshot))return firstError?firstError:-12;
        const uint64_t beginOffset=range->offset+at-range->begin;
        const uint64_t endOffset=beginOffset+right-at;
        at=right;
        auto* backing=snapshot.range.backing;
        // snapshot's backing owns the vnode until operation destruction, even
        // if another task unmaps/closes the final userspace reference during I/O.
        operation.Unlock();
        int error=-95;
        if(backing && backing->file && backing->fileVnode) {
            auto* vnode=backing->fileVnode;
            error=FileObjectWriteback(backing->file,vnode,beginOffset,endOffset);
            if(!error)error=vnode->ops->fsync?vnode->ops->fsync(vnode):-95;
        }
        if(!firstError && error)firstError=error;
    }
    return firstError;
}
int64_t UserMappingDiscardAnonymous(Process* actor, AddressSpaceState* space, uint64_t address, uint64_t length) {
    uint64_t end;
    if (!actor || !space || (address & 4095)) return -22;
    if (!length) return 0;
    if (!UserInterval(address,length,end)) return -22;
    for (;;) {
        struct Addresses { uint64_t* data=nullptr; ~Addresses() { kfree(data); } } addresses;
        MappingTransaction operation(actor,space);
        if (!operation.Lock()) return -12;
        for (uint64_t at=address;at<end;) {
            const auto* range=MappingRangeFind(space->ranges,at);
            if (!range) return -12;
            if ((range->kind != MappingKind::Anonymous && range->kind != MappingKind::Heap &&
                 range->kind != MappingKind::Stack) || range->backing ||
                range->sharing != MappingSharing::Private ||
                range->leafOwnership != MappingLeafOwnership::Managed) return -95;
            at=range->end<end?range->end:end;
        }
        uint64_t count=0;
        if (!operation.RangeLeafCount(address,end,count)) return -22;
        if (!count) return 0;
        const uint64_t generation=space->generation;
        operation.Unlock();
        if (count>~uint64_t{0}/sizeof(uint64_t)) return -12;
        addresses.data=static_cast<uint64_t*>(kmalloc(count*sizeof(uint64_t)));
        if (!addresses.data || !operation.Prepare(count,count) ||
            !operation.PrepareZeroed(MemTag::User,space->allocationPid)) return -12;
        if (!operation.Lock()) return -12;
        if (space->generation!=generation) continue;
        uint64_t actual=0;
        if (!VmmCollectRangeAddresses(space->pageTable,address,end,addresses.data,count,&actual)) continue;
        // Every replacement has storage and an existing page-table path before
        // the first detach. Never zero a shared CoW frame in place.
        for (uint64_t i=0;i<actual;++i) {
            const uint64_t at=addresses.data[i];
            const auto* range=MappingRangeFind(space->ranges,at);
            uint64_t flags=VMM_USER;
            if (range->protection & 2) flags|=VMM_WRITABLE;
            if (!(range->protection & 4)) flags|=VMM_NO_EXEC;
            if (!operation.Detach(VirtualAddress(at)) ||
                operation.Publish(i,VirtualAddress(at),flags,MemTag::User,space->allocationPid,
                                  range->protection!=0)!=VmmMapResult::Mapped)
                KernelPanic("mapping: preflighted anonymous discard failed");
        }
        return 0;
    }
}
static UserPageAccess CheckPage(AddressSpaceState* space,uint64_t address,uint32_t access,bool& cow) {
    cow = false;
    const auto* range=MappingRangeFind(space->ranges,address);
    if (!range || !access || (access & ~range->protection)) return UserPageAccess::Denied;
    uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(address));
    if (!(leaf & VMM_PRESENT)) return UserPageAccess::Missing;
    if (!(leaf & VMM_USER) || ((access & 4) && (leaf & VMM_NO_EXEC))) return UserPageAccess::Denied;
    if ((access & 2) && !(leaf & VMM_WRITABLE)) {
        cow = (leaf & PTE_COW_BIT) && range->sharing == MappingSharing::Private &&
            range->leafOwnership == MappingLeafOwnership::Managed;
        return UserPageAccess::Denied;
    }
    return UserPageAccess::Ready;
}
UserPageAccess UserMappingPageAccess(Process* actor, AddressSpaceState* space, uint64_t address,
                                     uint32_t access, bool resolveCow) {
    if (!actor || !space || space->kernel || address >= 0x0000800000000000ULL) return UserPageAccess::Denied;
    // Pin AS lifetime and cancellation protection before a potentially
    // blocking mutation lock. Read-only PTE/range checks need no file gates.
    MappingTransaction operation(actor,space);
    const uint64_t timingToken=MappingAccessTimingToken();
    const uint64_t timingBefore=(timingToken&1)?MappingAccessTimingClock():0;
    KMutexLock(&space->mutation);
    const uint64_t timingAcquired=(timingToken&1)?MappingAccessTimingClock():0;
    bool cow=false;auto status=CheckPage(space,address,access,cow);
    const uint64_t timingEnded=(timingToken&1)?MappingAccessTimingClock():0;
    KMutexUnlock(&space->mutation);
    if(timingToken&1)MappingAccessTimingRecord(timingToken,(access&2)!=0,timingBefore,
        timingAcquired,timingEnded,actor->pid,actor->incarnation);
    if (!resolveCow || !cow) return status;
    // Reacquire the existing full transaction and revalidate before CoW.
    if (!operation.Prepare(1,0)) return UserPageAccess::OutOfMemory;
    if(!operation.Lock())return UserPageAccess::OutOfMemory;status=CheckPage(space,address,access,cow);
    if (!cow) return status;
    auto result=VmmCowResolveWrite(space->pageTable,VirtualAddress(address),space->allocationPid);
    if (result.result == VmmCowResult::OutOfMemory) return UserPageAccess::OutOfMemory;
    if (result.result != VmmCowResult::Resolved) return CheckPage(space,address,access,cow);
    operation.RequireShootdown();
    if (result.retired && !operation.RetireFrame(result.retired)) KernelPanic("mapping: CoW retirement preflight mismatch");
    return UserPageAccess::Ready;
}
static int64_t SnapshotProgramBreak(AddressSpaceState* space) {
    KMutexLock(&space->mutation);auto value=space->programBreak;KMutexUnlock(&space->mutation);
    return static_cast<int64_t>(value);
}
int64_t UserMappingBreak(Process* actor,AddressSpaceState* space,uint64_t requested) {
    if (!actor || !space) return -12;
    for (;;) {
        MappingTransaction operation(actor,space);PmmThrottleForDrain();if(!operation.Lock())return SnapshotProgramBreak(space);
        const uint64_t previous=space->programBreak;
        if (!requested || requested<space->programBreakLow || requested>space->programBreakHigh || requested==previous)
            return static_cast<int64_t>(previous);
        const uint64_t oldEnd=(previous+4095)&~uint64_t{4095},newEnd=(requested+4095)&~uint64_t{4095};
        if (oldEnd==newEnd) { space->programBreak=requested;operation.Changed(false);return static_cast<int64_t>(requested); }
        uint64_t growth=newEnd>oldEnd?(newEnd-oldEnd)/4096:0,retired=0,extend=0;
        if (growth) {
            for (auto* node=space->ranges.head;node && node->range.begin<newEnd;node=node->next)
                if (node->range.end>oldEnd) return static_cast<int64_t>(previous);
            const auto* tail=MappingRangeFind(space->ranges,oldEnd-1);
            if (tail && tail->end==oldEnd && tail->kind==MappingKind::Heap && tail->protection==3 &&
                tail->maximumProtection==7 && tail->leafOwnership==MappingLeafOwnership::Managed &&
                tail->sharing==MappingSharing::Private && !tail->backing) extend=tail->identity;
        } else if (!operation.RangeLeafCount(newEnd,oldEnd,retired)) return static_cast<int64_t>(previous);
        const uint64_t generation=space->generation;operation.Unlock();
        if (!operation.Prepare(growth?growth:retired,growth) || !operation.PrepareRanges(3) ||
            (growth && (!operation.PrepareZeroed(MemTag::User,space->allocationPid) || !VmmPrepareRange(space->pageTable,oldEnd,newEnd)))) {
            if(!operation.Lock())return SnapshotProgramBreak(space);return static_cast<int64_t>(space->programBreak);
        }
        if(!operation.Lock())return SnapshotProgramBreak(space);if (space->generation!=generation || space->programBreak!=previous) continue;
        if (growth) {
            MappingRange heap{oldEnd,newEnd,0,0,nullptr,3,7,MappingKind::Heap,MappingSharing::Private,MappingLeafOwnership::Managed};
            if (extend ? !operation.ExtendRange(extend,newEnd) : !operation.InsertRange(heap)) return static_cast<int64_t>(previous);
            for (uint64_t i=0;i<growth;++i)
                if (operation.Publish(i,VirtualAddress(oldEnd+i*4096),VMM_USER|VMM_WRITABLE|VMM_NO_EXEC,
                                      MemTag::User,space->allocationPid)!=VmmMapResult::Mapped)
                    KernelPanic("mapping: preflighted heap publication failed");
        } else {
            if (!operation.CanReplaceRange(newEnd,oldEnd)) return static_cast<int64_t>(previous);
            if (!operation.ReplaceMapping(newEnd,oldEnd)) continue;
        }
        space->programBreak=requested;operation.Changed(false);return static_cast<int64_t>(requested);
    }
}
int64_t UserMappingResize(Process* actor,AddressSpaceState* space,uint64_t source,uint64_t oldLength,
                          uint64_t newLength,uint64_t flags,uint64_t destination,
                          UserRemapPreparation prepare,void* context) {
    constexpr uint64_t mayMove=1,fixed=2;
    uint64_t oldEnd,newEnd;
    if (!actor || !space || (flags & ~(mayMove|fixed)) || ((flags&fixed) && !(flags&mayMove)) ||
        !UserInterval(source,oldLength,oldEnd) || !UserInterval(0,newLength,newEnd)) return -22;
    oldLength=oldEnd-source;newLength=newEnd;
    const uint64_t movedLength=oldLength<newLength?oldLength:newLength;
    if (flags&fixed) {
        if (!UserInterval(destination,newLength,newEnd) || destination<VIRTUAL_NULL_GUARD ||
            (destination<oldEnd && source<newEnd)) return -22;
    }
    for (;;) {
        MappingTransaction operation(actor,space);PmmThrottleForDrain();if(!operation.Lock())return -12;
        MappingFaultSnapshot snapshot{};
        if (!operation.CaptureMapping(source,snapshot) || snapshot.range.begin!=source || snapshot.range.end!=oldEnd) return -14;
        if(snapshot.range.kind==MappingKind::Pageable)return -95;
        if (snapshot.range.kind==MappingKind::Device && newLength>oldLength) return -22;
        if (newLength==oldLength && !(flags&fixed)) return static_cast<int64_t>(source);
        bool shrink=newLength<oldLength && !(flags&fixed),inPlace=false;
        uint64_t target=source,count=0;
        if (shrink) {
            if (!operation.RangeLeafCount(source+newLength,oldEnd,count)) return -22;
        } else if (!(flags&fixed)) {
            inPlace=newLength<=0x0000800000000000ULL-source;
            for (auto* node=space->ranges.head;inPlace && node && node->range.begin<source+newLength;node=node->next)
                if (node->range.begin>=oldEnd) { inPlace=false;break; }
            if (!inPlace) {
                if (!(flags&mayMove)) return -12;
                UserMappingRequest request{};request.length=newLength;
                target=PickMappingAddress(space,request);if (!target) return -12;
            }
        } else {
            target=destination;
            if (!operation.RangeLeafCount(target,target+newLength,count)) return -22;
        }
        if (!shrink && !inPlace && newLength<oldLength) {
            uint64_t tail=0;if (!operation.RangeLeafCount(source+newLength,oldEnd,tail)) return -22;
            count+=tail;
        }
        operation.Unlock();
        if (!operation.Prepare(count,0) || !operation.PrepareRanges(3)) return -12;
        if (newLength>oldLength && prepare) {
            int error=prepare(snapshot.range,newLength,context);if (error) return error;
        }
        if (!shrink && !inPlace && !VmmPrepareMove(space->pageTable,source,source+movedLength,target)) return -12;
        if(!operation.Lock())return -12;if (!operation.ValidateMapping(snapshot,source)) continue;
        if (shrink) {
            if (!operation.CanReplaceRange(source+newLength,oldEnd)) return -12;
            if (!operation.ReplaceMapping(source+newLength,oldEnd)) continue;
            return static_cast<int64_t>(source);
        }
        if (inPlace) return operation.ExtendRange(snapshot.range.identity,source+newLength)?static_cast<int64_t>(source):-12;
        if (!VmmCanMoveRange(space->pageTable,source,source+movedLength,target,(flags&fixed)!=0)) continue;
        uint64_t destinationLeaves=0,tailLeaves=0;
        if (!operation.RangeLeafCount(target,target+newLength,destinationLeaves) ||
            (newLength<oldLength && !operation.RangeLeafCount(source+newLength,oldEnd,tailLeaves))) return -22;
        if (destinationLeaves+tailLeaves>count) continue;
        MappingRange replacement=snapshot.range;replacement.begin=target;replacement.end=target+newLength;replacement.identity=0;
        if (!operation.CanReplaceRange(target,target+newLength,&replacement) || !operation.CanReplaceRange(source,oldEnd)) return -12;
        if (!operation.ReplaceMapping(target,target+newLength,&replacement)) continue;
        if (!VmmMoveRange(space->pageTable,source,source+movedLength,target)) KernelPanic("mapping: preflighted move failed");
        if (!operation.ReplaceMapping(source,oldEnd)) KernelPanic("mapping: source retirement changed under lock");
        operation.RequireShootdown();
        if (target+newLength>space->mmapNext) space->mmapNext=target+newLength;
        return static_cast<int64_t>(target);
    }
}
}

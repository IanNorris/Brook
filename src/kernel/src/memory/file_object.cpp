#include "file_object.h"
#include "physical_memory.h"
#include "virtual_memory.h"
#include "heap.h"
#include "vfs.h"
#include "string.h"
#include "panic.h"
#include "spinlock.h"
#include "user_copy.h"
#ifndef BROOK_HOST_TEST
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#endif

namespace brook {
#ifdef BROOK_HOST_USER_COPY_TEST
bool FileObjectHostUserPointer(const void*);
#endif
struct FileObjectPage {
    uint64_t index;
    PhysicalAddress physical;
    FileObjectPage* next;
    bool writeExposed;
};
namespace {
IrqSpinLock writebackLock;
FileObject* writebackObjects;
void DropVnode(Vnode* vnode) {
    if(!vnode)return;
    uint32_t previous=__atomic_fetch_sub(&vnode->refCount,1,__ATOMIC_ACQ_REL);
    if(!previous)KernelPanic("file object: vnode ref underflow");
    if(previous==1)VfsClose(vnode);
}
struct Operation {
#ifndef BROOK_HOST_TEST
    Process* actor=ProcessCurrent();
    Operation() { if(actor) SchedulerKernelRegionEnter(actor); }
    ~Operation() { if(actor) SchedulerKernelRegionLeave(actor); }
#endif
};
struct Gate {
    FileObject* object;
    explicit Gate(FileObject* value):object(value) { KMutexLock(&object->gate); }
    ~Gate() { KMutexUnlock(&object->gate); }
};
struct OffsetGate {
    FileObject* object;
    explicit OffsetGate(FileObject* value):object(value) { KMutexLock(&object->ioOffsetGate); }
    ~OffsetGate() { KMutexUnlock(&object->ioOffsetGate); }
};
void RequireGate(FileObject* object) {
#ifndef BROOK_HOST_TEST
    if(!object->gate.locked || object->gate.owner!=ProcessCurrent())
        KernelPanic("file object: inode gate not owned");
#else
    (void)object;
#endif
}
FileObjectPage* Find(FileObject* object,uint64_t index) {
    for(auto* page=object->pages[index%64];page;page=page->next)
        if(page->index==index) return page;
    return nullptr;
}
void Release(FileObject* object,FileObjectPage* page) {
    while(page) {
        auto* next=page->next;PmmUnrefPage(page->physical);kfree(page);
        if(object->releasePageBytes)object->releasePageBytes(object->pageBudgetContext,4096+sizeof(FileObjectPage));
        page=next;
    }
}
void Overlay(FileObject* object,uint64_t offset,void* buffer,uint64_t length,bool write) {
    auto* bytes=static_cast<uint8_t*>(buffer);
    while(length) {
        uint64_t count=4096-(offset&4095);if(count>length)count=length;
        if(auto* page=Find(object,offset/4096)) {
            auto* resident=reinterpret_cast<uint8_t*>(PhysToVirt(page->physical).raw())+(offset&4095);
            if(write)memcpy(resident,bytes,count);else memcpy(bytes,resident,count);
        }
        offset+=count;bytes+=count;length-=count;
    }
}
void ZeroResidentGap(FileObject* object,uint64_t begin,uint64_t end) {
    for(auto* bucket:object->pages)for(auto* page=bucket;page;page=page->next) {
        const uint64_t offset=page->index*4096;
        if(offset>=end || (offset<begin && begin-offset>=4096))continue;
        const uint64_t left=begin>offset?begin-offset:0;
        const uint64_t right=end-offset<4096?end-offset:4096;
        memset(reinterpret_cast<uint8_t*>(PhysToVirt(page->physical).raw())+left,0,right-left);
    }
}
bool UserPointer(const void* pointer) {
#ifdef BROOK_HOST_USER_COPY_TEST
    return FileObjectHostUserPointer(pointer);
#elif defined(BROOK_HOST_TEST)
    (void)pointer;return false;
#else
    return reinterpret_cast<uint64_t>(pointer)<0x0000800000000000ULL;
#endif
}
uint64_t CopyOut(void* destination,const void* source,uint64_t length) {
#if !defined(BROOK_HOST_TEST) || defined(BROOK_HOST_USER_COPY_TEST)
    return CopyToUserBytes(reinterpret_cast<uint64_t>(destination),source,length);
#else
    memcpy(destination,source,length);return length;
#endif
}
uint64_t CopyIn(void* destination,const void* source,uint64_t length) {
#if !defined(BROOK_HOST_TEST) || defined(BROOK_HOST_USER_COPY_TEST)
    return CopyFromUserBytes(destination,reinterpret_cast<uint64_t>(source),length);
#else
    memcpy(destination,source,length);return length;
#endif
}
int ReadResident(FileObject* object,Vnode* vnode,void* buffer,uint64_t length,uint64_t* offset) {
    Gate held(object);const uint64_t begin=*offset;
    if(!length)return 0;
    if(object->mappingError)return object->mappingError;
    int result=vnode->ops->read(vnode,buffer,length,offset);
    if(result>0)Overlay(object,begin,buffer,static_cast<uint64_t>(result),false);
    return result;
}
int WriteLocked(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset) {
    if(!length)return 0;
    if(object->mappingError)return object->mappingError;
    const uint64_t begin=*offset;
    if(length>~uint64_t{0}-begin)return -22;
    bool resident=false;
    for(auto* bucket:object->pages)if(bucket){resident=true;break;}
    VnodeStat before{};
    if(resident) {
        const int error=vnode->ops->stat(vnode,&before);
        if(error)return error;
    }
    int result=vnode->ops->write(vnode,buffer,length,offset);
    if(result>0) {
        if(static_cast<uint64_t>(result)>length)return -5;
        // Stores into the old partial-page EOF tail are not valid file data.
        // A later extending pwrite must not turn them into file contents when
        // canonical pages overlay reads or are eventually written back.
        if(resident && begin+static_cast<uint64_t>(result)>before.size)
            ZeroResidentGap(object,before.size,begin+static_cast<uint64_t>(result));
        Overlay(object,begin,const_cast<void*>(buffer),static_cast<uint64_t>(result),true);
        ++object->generation;
    }
    return result;
}
int WriteResident(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset) {
    Gate held(object);return WriteLocked(object,vnode,buffer,length,offset);
}
}
void FileObjectInit(FileObject* object) {
    static uint64_t nextIdentity;
    object->recordLocks.lock.next=object->recordLocks.lock.serving=0;
    object->recordLocks.head=nullptr;
    KMutexInit(&object->gate);KMutexInit(&object->ioOffsetGate);object->generation=1;
    object->mappingError=0;object->privateValidPrefix=~uint64_t{0};
    uint64_t previous=__atomic_load_n(&nextIdentity,__ATOMIC_RELAXED);
    for(;;) {
        if(previous>=((uint64_t{1}<<62)-1))KernelPanic("file object: ordering identity exhausted");
        if(__atomic_compare_exchange_n(&nextIdentity,&previous,previous+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))break;
    }
    object->orderId=previous+1;
    object->mappings=nullptr;
    object->mappingRegistrations=0;object->writebackVnode=nullptr;object->writebackNext=nullptr;
    object->pageBudgetContext=nullptr;object->reservePageBytes=nullptr;object->releasePageBytes=nullptr;
    object->prepareBackingPage=nullptr;
    object->growMappedBacking=nullptr;
    for(auto& bucket:object->pages)bucket=nullptr;
}
void FileObjectDispose(FileObject* object) {
    if(object->recordLocks.head)KernelPanic("file object: disposing record locks");
    if(object->mappings || object->mappingRegistrations || object->writebackVnode)
        KernelPanic("file object: disposing retained inode");
    for(auto& bucket:object->pages) { auto* pages=bucket;bucket=nullptr;Release(object,pages); }
}
static PhysicalAddress GetPageLocked(FileObject* object,Vnode* vnode,uint64_t index,bool load,int* error,bool* loaded,
                                     const VnodeStat* knownStat=nullptr) {
    if(error)*error=0;
    if(loaded)*loaded=false;
    if(object->mappingError) { if(error)*error=object->mappingError;return PhysicalAddress{}; }
    if(index>~uint64_t{0}/4096) { if(error)*error=-22;return PhysicalAddress{}; }
    if(auto* page=Find(object,index)) { PmmRefPage(page->physical);return page->physical; }
    if(!load)return PhysicalAddress{};
    VnodeStat stat{};int result=0;
    if(knownStat)stat=*knownStat;else result=vnode->ops->stat(vnode,&stat);
    if(result || index*4096>=stat.size) { if(error)*error=result?result:-6;return PhysicalAddress{}; }
    if(object->prepareBackingPage) {
        int prepared=object->prepareBackingPage(object->pageBudgetContext,index);
        if(prepared) {if(error)*error=prepared;return PhysicalAddress{};}
    }
    constexpr uint64_t charge=4096+sizeof(FileObjectPage);
    if(object->reservePageBytes && !object->reservePageBytes(object->pageBudgetContext,charge)) {
        if(error)*error=-28;return PhysicalAddress{};
    }
    auto rollback=[&] {if(object->releasePageBytes)object->releasePageBytes(object->pageBudgetContext,charge);};
    auto* page=static_cast<FileObjectPage*>(kmalloc(sizeof(FileObjectPage)));
    if(!page) { rollback();if(error)*error=-12;return PhysicalAddress{}; }
    auto physical=PmmAllocPage(MemTag::User,KernelPid);
    if(!physical) { kfree(page);rollback();if(error)*error=-12;return PhysicalAddress{}; }
    auto* bytes=reinterpret_cast<void*>(PhysToVirt(physical).raw());memset(bytes,0,4096);
    uint64_t offset=index*4096;
    uint64_t required=stat.size-offset;if(required>4096)required=4096;
    uint64_t done=0;
    while(done<required) {
        result=vnode->ops->read(vnode,static_cast<uint8_t*>(bytes)+done,required-done,&offset);
        if(result<=0 || static_cast<uint64_t>(result)>required-done) {
            PmmUnrefPage(physical);kfree(page);rollback();
            if(error)*error=result<0?result:-5;
            return PhysicalAddress{};
        }
        done+=static_cast<uint64_t>(result);
    }
    // Loading is serialized by this inode's gate; no partially read page is
    // published and competing faults retain this same completed frame.
    *page={index,physical,object->pages[index%64],false};object->pages[index%64]=page;
    PmmRefPage(physical);if(loaded)*loaded=true;return physical;
}
PhysicalAddress FileObjectGetPage(FileObject* object,Vnode* vnode,uint64_t index,bool load,int* error,bool* loaded) {
    [[maybe_unused]] Operation operation;
    Gate held(object);
    return GetPageLocked(object,vnode,index,load,error,loaded);
}
int FileObjectGetPages(FileObject* object,Vnode* vnode,uint64_t index,uint32_t count,
                       PhysicalAddress* pages,bool* loaded) {
    [[maybe_unused]] Operation operation;
    if(loaded)*loaded=false;
    if(!pages || !count || count>FILE_OBJECT_PAGE_BATCH)return -22;
    for(uint32_t i=0;i<count;++i)pages[i]=PhysicalAddress{};
    constexpr uint64_t maximumIndex=~uint64_t{0}/4096;
    if(index>maximumIndex || count-1>maximumIndex-index)return -22;
    Gate held(object);
    int error=0;
    pages[0]=GetPageLocked(object,vnode,index,true,&error,loaded);
    if(!pages[0])return error?error:-5;
    // Neighbors are optional cache hits: never load them and do not stop at
    // a hole. Every returned slot owns a separate caller reference.
    for(uint32_t i=1;i<count;++i)
        if(auto* page=Find(object,index+i)) {
            PmmRefPage(page->physical);pages[i]=page->physical;
        }
    return 0;
}
MappingFaultResult FileObjectFaultPages(FileObject* object,Vnode* vnode,uint64_t index,
                                       uint32_t capacity,PhysicalAddress* pages,bool* loaded) {
    [[maybe_unused]] Operation operation;
    if(loaded)*loaded=false;
    if(!pages || !capacity || capacity>FILE_OBJECT_PAGE_BATCH)return MappingFaultResult::Unhandled;
    for(uint32_t i=0;i<capacity;++i)pages[i]=PhysicalAddress{};
    if(index>~uint64_t{0}/4096)return MappingFaultResult::EndOfFile;
    Gate held(object);
    auto failure=[&](MappingFaultResult result,int error,uint64_t size) {
#ifndef BROOK_HOST_TEST
        static uint32_t reports;
        if(__atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<32)
            SerialPrintf("FILEFAULT pid=%u object=%lu index=%lu size=%lu error=%d result=%u quarantine=%d generation=%lu\n",
                ProcessCurrent()?ProcessCurrent()->pid:0,
                (unsigned long)object->orderId,(unsigned long)index,
                (unsigned long)size,error,(unsigned)result,object->mappingError,
                (unsigned long)object->generation);
#else
        (void)error;(void)size;
#endif
        return result;
    };
    if(object->mappingError)return failure(MappingFaultResult::IoError,object->mappingError,0);
    VnodeStat stat{};
    const int statResult=vnode->ops->stat(vnode,&stat);
    if(statResult)return failure(statResult==-12?MappingFaultResult::OutOfMemory:MappingFaultResult::IoError,statResult,0);
    if(index*4096>=stat.size)return failure(MappingFaultResult::EndOfFile,-6,stat.size);
    int error=0;pages[0]=GetPageLocked(object,vnode,index,true,&error,loaded,&stat);
    if(!pages[0])return failure(error==-12?MappingFaultResult::OutOfMemory:
                       MappingFaultResult::IoError,error,stat.size);
    for(uint32_t i=1;i<capacity;++i) {
        if(index+i>~uint64_t{0}/4096 || (index+i)*4096>=stat.size)break;
        if(auto* page=Find(object,index+i)) { PmmRefPage(page->physical);pages[i]=page->physical; }
    }
    return MappingFaultResult::Resolved;
}
int FileObjectRead(FileObject* object,Vnode* vnode,void* buffer,uint64_t length,uint64_t* offset) {
    [[maybe_unused]] Operation operation;
    OffsetGate offsetHeld(object);
    if(!length || !UserPointer(buffer))return ReadResident(object,vnode,buffer,length,offset);
    constexpr uint64_t maximum=64*1024;
    uint64_t capacity=length<maximum?length:maximum;
    auto* bounce=static_cast<uint8_t*>(kmalloc(capacity));if(!bounce)return -12;
    uint64_t done=0;int result=0;
    while(done<length) {
        uint64_t count=length-done;if(count>capacity)count=capacity;
        uint64_t position=*offset;
        result=ReadResident(object,vnode,bounce,count,&position);
        if(result<=0)break;
        // Copyout may fault/CoW. The file gate and filesystem locks are gone.
        const uint64_t copied=CopyOut(static_cast<uint8_t*>(buffer)+done,bounce,static_cast<uint64_t>(result));
        *offset+=copied;done+=copied;
        if(copied<static_cast<uint64_t>(result)) { result=-14;break; }
        if(static_cast<uint64_t>(result)<count)break;
    }
    kfree(bounce);return done?static_cast<int>(done):result;
}
int FileObjectWrite(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset) {
    [[maybe_unused]] Operation operation;
    OffsetGate offsetHeld(object);
    if(!length || !UserPointer(buffer))return WriteResident(object,vnode,buffer,length,offset);
    constexpr uint64_t maximum=64*1024;
    uint64_t capacity=length<maximum?length:maximum;
    auto* bounce=static_cast<uint8_t*>(kmalloc(capacity));if(!bounce)return -12;
    uint64_t done=0;int result=0;
    while(done<length) {
        uint64_t count=length-done;if(count>capacity)count=capacity;
        // Copyin can fault on this file itself, so it precedes the file gate.
        const uint64_t copied=CopyIn(bounce,static_cast<const uint8_t*>(buffer)+done,count);
        if(!copied) { result=-14;break; }
        result=WriteResident(object,vnode,bounce,copied,offset);
        if(result<=0)break;
        done+=static_cast<uint64_t>(result);if(static_cast<uint64_t>(result)<count)break;
    }
    kfree(bounce);return done?static_cast<int>(done):result;
}
int FileObjectAppend(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset) {
    [[maybe_unused]] Operation operation;
    if(!length)return 0;
    if(UserPointer(buffer))return -22; // Never fault/copy user memory under gate.
    OffsetGate offsetHeld(object);
    Gate held(object);
    if(object->mappingError)return object->mappingError;
    VnodeStat stat{};int result=vnode->ops->stat(vnode,&stat);
    if(result)return result;
    uint64_t position=stat.size;
    result=WriteLocked(object,vnode,buffer,length,&position);
    if(result>0)*offset=position;
    return result;
}
bool FileObjectIsCanonicalLocked(FileObject* object,uint64_t index,PhysicalAddress physical) {
    RequireGate(object);
    auto* page=Find(object,index);
    return page && physical && page->physical==physical;
}
void FileObjectQuarantineLocked(FileObject* object,int error,uint64_t lastKnownPrefix) {
    RequireGate(object);
    if(error>=0)KernelPanic("file object: invalid mapping quarantine error");
    if(object->generation==~uint64_t{0})KernelPanic("file object: generation exhausted");
    object->mappingError=error;
    if(lastKnownPrefix<object->privateValidPrefix)object->privateValidPrefix=lastKnownPrefix;
    ++object->generation;
}
void FileObjectCompleteMappingRecoveryLocked(FileObject* object) {
    RequireGate(object);
    if(!object->mappingError)KernelPanic("file object: recovery without quarantine");
    if(object->generation==~uint64_t{0})KernelPanic("file object: generation exhausted");
    for(auto& bucket:object->pages) { auto* retired=bucket;bucket=nullptr;Release(object,retired); }
    object->mappingError=0;object->privateValidPrefix=~uint64_t{0};++object->generation;
}
bool FileObjectExposeWritableLocked(FileObject* object,Vnode* vnode,uint64_t index,PhysicalAddress physical) {
    RequireGate(object);
    if(object->mappingError)return false;
    auto* page=Find(object,index);
    if(!page || !physical || page->physical!=physical || !vnode || vnode->type!=VnodeType::File || !vnode->ops ||
       !vnode->ops->stat || !vnode->ops->write || !vnode->ops->file_object ||
       vnode->ops->file_object(vnode)!=object)return false;
    if(!object->writebackVnode) {
        VfsRetain(vnode);
        const uint64_t flags=IrqSpinLockAcquire(&writebackLock);
        object->writebackVnode=vnode;object->writebackNext=writebackObjects;writebackObjects=object;
        IrqSpinLockRelease(&writebackLock,flags);
    }
    page->writeExposed=true;
    return true;
}
bool FileObjectExposeRangeLocked(FileObject* object,Vnode* vnode,uint64_t begin,uint64_t end) {
    RequireGate(object);
    if(begin>end || object->mappingError || !vnode || vnode->type!=VnodeType::File ||
       !vnode->ops || !vnode->ops->stat || !vnode->ops->write || !vnode->ops->file_object ||
       vnode->ops->file_object(vnode)!=object)return false;
    if(begin==end)return true;
    for(auto* bucket:object->pages)for(auto* page=bucket;page;page=page->next) {
        const uint64_t offset=page->index*4096;
        if(offset<end && (offset>=begin || begin-offset<4096) &&
           !FileObjectExposeWritableLocked(object,vnode,page->index,page->physical))return false;
    }
    return true;
}
static int WritebackLocked(FileObject* object,Vnode* vnode,uint64_t begin,uint64_t end,uint8_t* snapshot) {
    if(object->mappingError)return object->mappingError;
    int result=0;
    VnodeStat stat{};result=vnode->ops->stat(vnode,&stat);
    if(!result)for(auto* bucket:object->pages) {
        for(auto* page=bucket;page;page=page->next) {
            const uint64_t offset=page->index*4096;
            if(!page->writeExposed || offset>=stat.size || offset>=end ||
               (offset<begin && begin-offset>=4096))continue;
            uint64_t count=stat.size-offset;if(count>4096)count=4096;
            memcpy(snapshot,reinterpret_cast<void*>(PhysToVirt(page->physical).raw()),count);
            uint64_t done=0;
            while(done<count) {
                uint64_t position=offset+done;
                int written=vnode->ops->write(vnode,snapshot+done,count-done,&position);
                if(written<=0 || static_cast<uint64_t>(written)>count-done ||
                   position!=offset+done+static_cast<uint64_t>(written)) {
                    result=written<0?written:-5;break;
                }
                done+=static_cast<uint64_t>(written);
            }
            // Never clear exposure: a store racing this snapshot, or any
            // later store through an alias, must be captured by a retry.
            if(result)break;
        }
        if(result)break;
    }
    return result;
}
int FileObjectWriteback(FileObject* object,Vnode* vnode,uint64_t begin,uint64_t end) {
    [[maybe_unused]] Operation operation;
    if(begin>end)return -22;
    if(begin==end)return 0;
    if(!vnode || !vnode->ops || !vnode->ops->stat || !vnode->ops->write)return -95;
    { Gate held(object);if(object->mappingError)return object->mappingError;if(!object->writebackVnode)return 0; }
    // Allocate before taking the file gate. The backend never sees live mapped
    // memory: a concurrent user store can change it during filesystem I/O.
    auto* snapshot=static_cast<uint8_t*>(kmalloc(4096));if(!snapshot)return -12;
    int result;
    { Gate held(object);result=WritebackLocked(object,vnode,begin,end,snapshot); }
    kfree(snapshot);return result;
}
int FileObjectWritebackAll() {
    [[maybe_unused]] Operation operation;
    uint64_t ceiling=0;
    uint64_t flags=IrqSpinLockAcquire(&writebackLock);
    for(auto* object=writebackObjects;object;object=object->writebackNext)
        if(object->orderId>ceiling)ceiling=object->orderId;
    IrqSpinLockRelease(&writebackLock,flags);
    if(!ceiling)return 0;
    auto* snapshot=static_cast<uint8_t*>(kmalloc(4096));if(!snapshot)return -12;
    int result=0;uint64_t cursor=0;
    for(;;) {
        FileObject* object=nullptr;Vnode* vnode=nullptr;
        flags=IrqSpinLockAcquire(&writebackLock);
        for(auto* candidate=writebackObjects;candidate;candidate=candidate->writebackNext)
            if(candidate->orderId>cursor && candidate->orderId<=ceiling &&
               (!object || candidate->orderId<object->orderId))object=candidate;
        if(object) { vnode=object->writebackVnode;VfsRetain(vnode);cursor=object->orderId; }
        IrqSpinLockRelease(&writebackLock,flags);
        if(!object)break;
        Vnode* retired=nullptr;
        {
            Gate held(object);
            // Another flush may have dequeued it while our temporary vnode pin
            // kept the object alive. Never consume a newly queued generation
            // without first flushing it under this same gate.
            if(object->writebackVnode) {
                const int error=WritebackLocked(object,vnode,0,~uint64_t{0},snapshot);
                if(!result && error)result=error;
                // Quarantined cache was flushed before the failed resize and
                // has accepted no writes since. Once all aliases retire it is
                // stale data, not pending dirty output: report the error but
                // release this queue pin instead of retaining an orphan forever.
                if((!error || object->mappingError) && !__atomic_load_n(&object->mappingRegistrations,__ATOMIC_ACQUIRE)) {
                    flags=IrqSpinLockAcquire(&writebackLock);
                    auto** link=&writebackObjects;
                    while(*link && *link!=object)link=&(*link)->writebackNext;
                    if(!*link)KernelPanic("file object: writeback queue missing");
                    *link=object->writebackNext;object->writebackNext=nullptr;
                    retired=object->writebackVnode;object->writebackVnode=nullptr;
                    IrqSpinLockRelease(&writebackLock,flags);
                    if(object->mappingError) {
                        for(auto& bucket:object->pages) { auto* pages=bucket;bucket=nullptr;Release(object,pages); }
                    } else for(auto* bucket:object->pages)for(auto* page=bucket;page;page=page->next)page->writeExposed=false;
                }
            }
        }
        DropVnode(retired);DropVnode(vnode);
    }
    kfree(snapshot);return result;
}
#ifndef BROOK_HOST_TEST
namespace {
KMutex writebackWorkerGate;
bool writebackWorkerStarted=false;
bool writebackWorkerEnabled=false;
void WritebackWorker(void*) {
    int lastError=0;
    for(;;) {
        SchedulerSleepMs(5000);
        KMutexLock(&writebackWorkerGate);
        if(__atomic_load_n(&writebackWorkerEnabled,__ATOMIC_ACQUIRE)) {
            auto flags=IrqSpinLockAcquire(&writebackLock);
            const bool pending=writebackObjects!=nullptr;
            IrqSpinLockRelease(&writebackLock,flags);
            // Canonical snapshots precede filesystem/device durability. Empty
            // queues do not trigger periodic flushes of unrelated mounts.
            const int error=pending?VfsSync(nullptr):0;
            if(error && error!=lastError)SerialPrintf("file-writeback: periodic sync failed: %d; backing retained for retry\n",error);
            lastError=error;
        }
        KMutexUnlock(&writebackWorkerGate);
    }
}
}
void FileObjectStartWriteback() {
    if(writebackWorkerStarted)return;
    KMutexInit(&writebackWorkerGate);
    auto* worker=KernelThreadCreate("file-writeback",WritebackWorker,nullptr,3);
    if(!worker)KernelPanic("file writeback: could not create worker");
    writebackWorkerStarted=true;
    __atomic_store_n(&writebackWorkerEnabled,true,__ATOMIC_RELEASE);
    SchedulerAddProcess(worker);
}
void FileObjectStopWriteback() {
    if(!writebackWorkerStarted)return;
    __atomic_store_n(&writebackWorkerEnabled,false,__ATOMIC_RELEASE);
    KMutexLock(&writebackWorkerGate);KMutexUnlock(&writebackWorkerGate);
}
#endif
int FileObjectGrowMapped(FileObject* object,Vnode* vnode,uint64_t length) {
    [[maybe_unused]] Operation operation;
    Gate held(object);
    if(object->mappingError)return object->mappingError;
    if(!object->growMappedBacking)return -16;
    VnodeStat before{};int result=vnode->ops->stat(vnode,&before);
    if(result)return result;
    // Ordinary writes can extend EOF independently of the VFS first-pin gate.
    // Check the current size here so a racing extension cannot turn growth
    // into shrink. Aligned EOF excludes an old, user-modifiable partial tail.
    if(length<=before.size || (before.size&4095))return -16;
    result=object->growMappedBacking(vnode,length);
    // Do not flush or evict old canonical pages: live PTEs borrow these frames.
    // Even an error can commit the larger EOF. Revalidate prepared faults while
    // retaining the still-valid old prefix and every mapping/exposure lease.
    ++object->generation;
    return result;
}
int FileObjectTruncateUnmapped(FileObject* object,Vnode* vnode,uint64_t length) {
    [[maybe_unused]] Operation operation;
    // The caller excludes mappings and first-pin publication. Persist any
    // pending exposure before evicting the canonical cache, even on an error.
    int writeback=FileObjectWriteback(object,vnode,0,~uint64_t{0});if(writeback)return writeback;
    FileObjectPage* retired[64];memset(retired,0,sizeof(retired));int result;
    {
        Gate held(object);
        if(object->mappingError)return object->mappingError;
        result=vnode->ops->truncate(vnode,length);
        // A backend error does not imply rollback: ext2 can modify a tail or
        // inode before a later write/free fails. Never reopen stale cached
        // contents after an attempted resize, even when preserving its errno.
        for(unsigned i=0;i<64;++i) { retired[i]=object->pages[i];object->pages[i]=nullptr; }
        ++object->generation;
    }
    for(auto* bucket:retired)Release(object,bucket);
    return result;
}
}

#include "file_object.h"
#include "physical_memory.h"
#include "virtual_memory.h"
#include "heap.h"
#include "vfs.h"
#include "string.h"
#include "panic.h"
#ifndef BROOK_HOST_TEST
#include "process.h"
#include "scheduler.h"
#endif

namespace brook {
struct FileObjectPage {
    uint64_t index;
    PhysicalAddress physical;
    FileObjectPage* next;
};
namespace {
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
FileObjectPage* Find(FileObject* object,uint64_t index) {
    for(auto* page=object->pages[index%64];page;page=page->next)
        if(page->index==index) return page;
    return nullptr;
}
void Release(FileObjectPage* page) {
    while(page) { auto* next=page->next;PmmUnrefPage(page->physical);kfree(page);page=next; }
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
bool UserPointer(const void* pointer) {
#ifdef BROOK_HOST_TEST
    (void)pointer;return false;
#else
    return reinterpret_cast<uint64_t>(pointer)<0x0000800000000000ULL;
#endif
}
int ReadResident(FileObject* object,Vnode* vnode,void* buffer,uint64_t length,uint64_t* offset) {
    Gate held(object);const uint64_t begin=*offset;
    int result=vnode->ops->read(vnode,buffer,length,offset);
    if(result>0)Overlay(object,begin,buffer,static_cast<uint64_t>(result),false);
    return result;
}
int WriteResident(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset) {
    Gate held(object);const uint64_t begin=*offset;
    int result=vnode->ops->write(vnode,buffer,length,offset);
    if(result>0) {
        Overlay(object,begin,const_cast<void*>(buffer),static_cast<uint64_t>(result),true);
        ++object->generation;
    }
    return result;
}
}
void FileObjectInit(FileObject* object) {
    static uint64_t nextIdentity;
    KMutexInit(&object->gate);object->generation=1;
    object->orderId=__atomic_add_fetch(&nextIdentity,1,__ATOMIC_RELAXED);
    if(!object->orderId)KernelPanic("file object: ordering identity exhausted");
    object->mappings=nullptr;
    for(auto& bucket:object->pages)bucket=nullptr;
}
void FileObjectDispose(FileObject* object) {
    if(object->mappings)KernelPanic("file object: disposing mapped inode");
    for(auto& bucket:object->pages) { auto* pages=bucket;bucket=nullptr;Release(pages); }
}
static PhysicalAddress GetPageLocked(FileObject* object,Vnode* vnode,uint64_t index,bool load,int* error,bool* loaded) {
    if(error)*error=0;
    if(loaded)*loaded=false;
    if(index>~uint64_t{0}/4096) { if(error)*error=-22;return PhysicalAddress{}; }
    if(auto* page=Find(object,index)) { PmmRefPage(page->physical);return page->physical; }
    if(!load)return PhysicalAddress{};
    VnodeStat stat{};int result=vnode->ops->stat(vnode,&stat);
    if(result || index*4096>=stat.size) { if(error)*error=result?result:-6;return PhysicalAddress{}; }
    auto* page=static_cast<FileObjectPage*>(kmalloc(sizeof(FileObjectPage)));
    if(!page) { if(error)*error=-12;return PhysicalAddress{}; }
    auto physical=PmmAllocPage(MemTag::User,KernelPid);
    if(!physical) { kfree(page);if(error)*error=-12;return PhysicalAddress{}; }
    auto* bytes=reinterpret_cast<void*>(PhysToVirt(physical).raw());memset(bytes,0,4096);
    uint64_t offset=index*4096;
    uint64_t required=stat.size-offset;if(required>4096)required=4096;
    uint64_t done=0;
    while(done<required) {
        result=vnode->ops->read(vnode,static_cast<uint8_t*>(bytes)+done,required-done,&offset);
        if(result<=0 || static_cast<uint64_t>(result)>required-done) {
            PmmUnrefPage(physical);kfree(page);
            if(error)*error=result<0?result:-5;
            return PhysicalAddress{};
        }
        done+=static_cast<uint64_t>(result);
    }
    // Loading is serialized by this inode's gate; no partially read page is
    // published and competing faults retain this same completed frame.
    *page={index,physical,object->pages[index%64]};object->pages[index%64]=page;
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
int FileObjectRead(FileObject* object,Vnode* vnode,void* buffer,uint64_t length,uint64_t* offset) {
    [[maybe_unused]] Operation operation;
    if(!length || !UserPointer(buffer))return ReadResident(object,vnode,buffer,length,offset);
    constexpr uint64_t maximum=64*1024;
    uint64_t capacity=length<maximum?length:maximum;
    auto* bounce=static_cast<uint8_t*>(kmalloc(capacity));if(!bounce)return -12;
    uint64_t done=0;int result=0;
    while(done<length) {
        uint64_t count=length-done;if(count>capacity)count=capacity;
        result=ReadResident(object,vnode,bounce,count,offset);
        if(result<=0)break;
        // Copyout may fault/CoW. The file gate and filesystem locks are gone.
        memcpy(static_cast<uint8_t*>(buffer)+done,bounce,static_cast<uint64_t>(result));
        done+=static_cast<uint64_t>(result);if(static_cast<uint64_t>(result)<count)break;
    }
    kfree(bounce);return done?static_cast<int>(done):result;
}
int FileObjectWrite(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset) {
    [[maybe_unused]] Operation operation;
    if(!length || !UserPointer(buffer))return WriteResident(object,vnode,buffer,length,offset);
    constexpr uint64_t maximum=64*1024;
    uint64_t capacity=length<maximum?length:maximum;
    auto* bounce=static_cast<uint8_t*>(kmalloc(capacity));if(!bounce)return -12;
    uint64_t done=0;int result=0;
    while(done<length) {
        uint64_t count=length-done;if(count>capacity)count=capacity;
        // Copyin can fault on this file itself, so it precedes the file gate.
        memcpy(bounce,static_cast<const uint8_t*>(buffer)+done,count);
        result=WriteResident(object,vnode,bounce,count,offset);
        if(result<=0)break;
        done+=static_cast<uint64_t>(result);if(static_cast<uint64_t>(result)<count)break;
    }
    kfree(bounce);return done?static_cast<int>(done):result;
}
int FileObjectTruncateUnmapped(FileObject* object,Vnode* vnode,uint64_t length) {
    [[maybe_unused]] Operation operation;
    FileObjectPage* retired[64];memset(retired,0,sizeof(retired));int result;
    {
        Gate held(object);result=vnode->ops->truncate(vnode,length);
        // A backend error does not imply rollback: ext2 can modify a tail or
        // inode before a later write/free fails. Never reopen stale cached
        // contents after an attempted resize, even when preserving its errno.
        for(unsigned i=0;i<64;++i) { retired[i]=object->pages[i];object->pages[i]=nullptr; }
        ++object->generation;
    }
    for(auto* bucket:retired)Release(bucket);
    return result;
}
}

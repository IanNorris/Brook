#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "memory/file_object.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"
#include "vfs.h"
#include "panic.h"
#include "user_copy.h"
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::abort(); } } while (0)
using namespace brook;
namespace {
std::mutex locksGuard,framesGuard;
std::unordered_map<KMutex*,std::unique_ptr<std::mutex>> locks;
std::unordered_map<uint64_t,unsigned> refs;
std::atomic<unsigned> heapBlocks{0},gateAcquires{0};
unsigned closedVnodes=0;
std::atomic<bool> failHeap{false},failFrame{false};
std::mutex writeHookGuard;
std::condition_variable writeHookCv;
bool writeHookEntered=false,writeHookReleased=false;
uint64_t userBegin=0,userEnd=0;
std::atomic<uint64_t> copyLimit{~uint64_t{0}};
bool blockCopy=false,copyEntered=false,copyReleased=false;
std::mutex copyGuard;std::condition_variable copyCv;
struct Backend {
    FileObject object;
    std::vector<unsigned char> bytes;
    unsigned reads=0;
    unsigned writes=0,errorAtWrite=0,zeroAtWrite=0;
    bool failRead=false,failWrite=false,failTruncate=false,failAfterTruncate=false,failStat=false;
    int readError=-5,statError=-5;
    unsigned char* redirty=nullptr;
    bool blockWrite=false;
    unsigned writeLimit=~0u,readLimit=~0u,zeroAtRead=0,errorAtRead=0;
};
int read_backend(Vnode* vnode,void* buffer,uint64_t length,uint64_t* offset) {
    auto* backend=static_cast<Backend*>(vnode->priv);++backend->reads;
    if(backend->failRead || backend->reads==backend->errorAtRead)return backend->readError;
    if(backend->reads==backend->zeroAtRead)return 0;
    if(*offset>=backend->bytes.size())return 0;
    size_t count=backend->bytes.size()-*offset;if(count>length)count=length;
    if(count>backend->readLimit)count=backend->readLimit;
    std::memcpy(buffer,backend->bytes.data()+*offset,count);*offset+=count;return static_cast<int>(count);
}
int write_backend(Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset) {
    auto* backend=static_cast<Backend*>(vnode->priv);++backend->writes;
    if(backend->failWrite || backend->writes==backend->errorAtWrite)return -5;
    if(backend->writes==backend->zeroAtWrite)return 0;
    if(backend->redirty) {
        CHECK(buffer!=backend->redirty);
        *backend->redirty=0xB9;backend->redirty=nullptr;
    }
    if(backend->blockWrite) {
        std::unique_lock<std::mutex> held(writeHookGuard);
        writeHookEntered=true;writeHookCv.notify_all();
        writeHookCv.wait(held,[]{return writeHookReleased;});
    }
    if(length>backend->writeLimit)length=backend->writeLimit;
    if(*offset+length>backend->bytes.size())backend->bytes.resize(*offset+length);
    std::memcpy(backend->bytes.data()+*offset,buffer,length);*offset+=length;return static_cast<int>(length);
}
int stat_backend(Vnode* vnode,VnodeStat* stat) {
    auto* backend=static_cast<Backend*>(vnode->priv);if(backend->failStat)return backend->statError;
    stat->size=backend->bytes.size();return 0;
}
int truncate_backend(Vnode* vnode,uint64_t length) {
    auto* backend=static_cast<Backend*>(vnode->priv);if(backend->failTruncate)return -5;
    backend->bytes.resize(length);
    if(backend->failAfterTruncate) {
        // Model a committed tail/inode change followed by a later I/O error.
        if(length)backend->bytes[0]=0xA7;
        return -5;
    }
    return 0;
}
FileObject* object_backend(Vnode* vnode) { return &static_cast<Backend*>(vnode->priv)->object; }
const VnodeOps operations={nullptr,read_backend,write_backend,nullptr,nullptr,stat_backend,nullptr,nullptr,nullptr,truncate_backend,object_backend};
struct Barrier {
    std::mutex guard;std::condition_variable cv;unsigned arrived=0;
    void wait() { std::unique_lock<std::mutex> lock(guard);++arrived;cv.notify_all();cv.wait(lock,[&]{return arrived==8;}); }
};
}
namespace brook {
bool FileObjectHostUserPointer(const void* pointer) {
    auto address=reinterpret_cast<uint64_t>(pointer);return address>=userBegin&&address<userEnd;
}
static uint64_t UserCopyTest(void* destination,const void* source,uint64_t length) {
    uint64_t limit=copyLimit.exchange(~uint64_t{0});if(length>limit)length=limit;
    if(blockCopy) {
        std::unique_lock<std::mutex> lock(copyGuard);copyEntered=true;copyCv.notify_all();
        copyCv.wait(lock,[]{return copyReleased;});
    }
    std::memcpy(destination,source,length);return length;
}
uint64_t CopyFromUserBytes(void* destination,uint64_t source,uint64_t length) {
    return UserCopyTest(destination,reinterpret_cast<void*>(source),length);
}
uint64_t CopyToUserBytes(uint64_t destination,const void* source,uint64_t length) {
    return UserCopyTest(reinterpret_cast<void*>(destination),source,length);
}
extern "C" void VfsClose(Vnode* vnode) {
    CHECK(!vnode->refCount);++closedVnodes;
    FileObjectDispose(vnode->ops->file_object(vnode));
}
void KMutexInit(KMutex* mutex) { std::lock_guard<std::mutex> lock(locksGuard);locks[mutex]=std::make_unique<std::mutex>(); }
void KMutexLock(KMutex* mutex) { ++gateAcquires;std::mutex* lock;{std::lock_guard<std::mutex> held(locksGuard);lock=locks.at(mutex).get();}lock->lock(); }
void KMutexUnlock(KMutex* mutex) { std::mutex* lock;{std::lock_guard<std::mutex> held(locksGuard);lock=locks.at(mutex).get();}lock->unlock(); }
extern "C" void* kmalloc(uint64_t bytes) { if(failHeap)return nullptr;auto* memory=std::malloc(bytes);if(memory)++heapBlocks;return memory; }
extern "C" void kfree(void* memory) { if(memory){CHECK(heapBlocks.fetch_sub(1)>0);std::free(memory);} }
PhysicalAddress PmmAllocPage(MemTag,uint16_t) {
    if(failFrame)return PhysicalAddress{};
    void* memory=nullptr;CHECK(posix_memalign(&memory,4096,4096)==0);
    auto address=reinterpret_cast<uint64_t>(memory);std::lock_guard<std::mutex> lock(framesGuard);refs[address]=1;return PhysicalAddress(address);
}
void PmmRefPage(PhysicalAddress page) { std::lock_guard<std::mutex> lock(framesGuard);CHECK(refs.at(page.raw())>0);++refs.at(page.raw()); }
void PmmUnrefPage(PhysicalAddress page) { std::lock_guard<std::mutex> lock(framesGuard);CHECK(refs.at(page.raw())>0);if(!--refs.at(page.raw())){refs.erase(page.raw());std::free(reinterpret_cast<void*>(page.raw()));} }
}
extern "C" __attribute__((noreturn)) void KernelPanic(const char*,...) { std::abort(); }
static void AppendTransactions() {
    CHECK(refs.empty()&&heapBlocks==0);
    Backend backend{};FileObjectInit(&backend.object);backend.bytes.resize(10,0x31);
    Vnode first{&operations,VnodeType::File,&backend,1,0};
    Vnode second{&operations,VnodeType::File,&backend,1,0};
    int error=0;auto page=FileObjectGetPage(&backend.object,&first,0,true,&error);
    CHECK(page&&error==0);
    auto* bytes=reinterpret_cast<unsigned char*>(page.raw());
    uint64_t offset=0;const unsigned char data[]={0x41,0x42,0x43};
    CHECK(FileObjectAppend(&backend.object,&first,data,3,&offset)==3&&offset==13);
    CHECK(bytes[9]==0x31&&bytes[10]==0x41&&bytes[12]==0x43);
    offset=1;backend.writeLimit=1;
    CHECK(FileObjectAppend(&backend.object,&second,data,3,&offset)==1&&offset==14&&bytes[13]==0x41);
    const auto generation=backend.object.generation;
    backend.failStat=true;offset=7;
    CHECK(FileObjectAppend(&backend.object,&second,data,3,&offset)==-5&&offset==7);
    backend.failStat=false;backend.failWrite=true;
    CHECK(FileObjectAppend(&backend.object,&second,data,3,&offset)==-5&&offset==7);
    backend.failWrite=false;backend.zeroAtWrite=backend.writes+1;
    CHECK(FileObjectAppend(&backend.object,&second,data,3,&offset)==0&&offset==7);
    CHECK(FileObjectAppend(&backend.object,&second,nullptr,0,&offset)==0&&offset==7);
    CHECK(backend.object.generation==generation);
    backend.zeroAtWrite=0;backend.writeLimit=~0u;
    PmmUnrefPage(page);
    CHECK(FileObjectTruncateUnmapped(&backend.object,&first,0)==0);
    std::vector<std::thread> writers;
    for(unsigned n=0;n<8;++n)writers.emplace_back([&,n] {
        // Independent vnode and deliberately stale offset for every record.
        Vnode vnode{&operations,VnodeType::File,&backend,1,0};
        unsigned char record[64];std::memset(record,n+1,sizeof(record));
        for(unsigned i=0;i<100;++i) {
            uint64_t stale=0;
            CHECK(FileObjectAppend(&backend.object,&vnode,record,sizeof(record),&stale)==64);
        }
    });
    for(auto& writer:writers)writer.join();
    CHECK(backend.bytes.size()==8*100*64);
    unsigned records[8]{};
    for(unsigned i=0;i<backend.bytes.size();i+=64) {
        unsigned value=backend.bytes[i];CHECK(value>=1&&value<=8);++records[value-1];
        for(unsigned j=0;j<64;++j)CHECK(backend.bytes[i+j]==value);
    }
    for(auto count:records)CHECK(count==100);
    // A truncation through another open cannot enter between EOF selection
    // and backend/cache publication. Block the append inside its backend.
    backend.blockWrite=true;writeHookEntered=false;writeHookReleased=false;
    std::thread appender([&] {
        uint64_t stale=0;CHECK(FileObjectAppend(&backend.object,&first,data,3,&stale)==3);
    });
    { std::unique_lock<std::mutex> lock(writeHookGuard);
      writeHookCv.wait(lock,[]{return writeHookEntered;}); }
    const auto before=gateAcquires.load();std::atomic<bool> truncated{false};
    std::thread truncater([&] {
        CHECK(FileObjectTruncateUnmapped(&backend.object,&second,0)==0);truncated=true;
    });
    while(gateAcquires.load()==before)std::this_thread::yield();
    CHECK(!truncated.load());
    { std::lock_guard<std::mutex> lock(writeHookGuard);writeHookReleased=true; }
    writeHookCv.notify_all();appender.join();truncater.join();
    CHECK(truncated.load()&&backend.bytes.empty());
    FileObjectDispose(&backend.object);CHECK(refs.empty()&&heapBlocks==0);
}
static void WritebackPages() {
    CHECK(refs.empty()&&heapBlocks==0);
    Backend backend{};FileObjectInit(&backend.object);backend.bytes.resize(8192+37,0x31);
    Vnode vnode{&operations,VnodeType::File,&backend,1,0};
    PhysicalAddress pages[3];int error=0;
    for(unsigned i=0;i<3;++i) {
        pages[i]=FileObjectGetPage(&backend.object,&vnode,i,true,&error);
        CHECK(pages[i]&&error==0);
    }
    auto* first=reinterpret_cast<unsigned char*>(pages[0].raw());
    auto* second=reinterpret_cast<unsigned char*>(pages[1].raw());
    auto* last=reinterpret_cast<unsigned char*>(pages[2].raw());
    first[0]=0x71;second[0]=0x72;last[36]=0x73;last[37]=0x74;
    // A resident page alone does not imply a writable shared exposure.
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,~uint64_t{0})==0&&backend.writes==0);
    KMutexLock(&backend.object.gate);
    CHECK(!FileObjectExposeWritableLocked(&backend.object,&vnode,0,pages[1]));
    CHECK(!FileObjectExposeWritableLocked(&backend.object,&vnode,3,pages[0]));
    CHECK(!FileObjectExposeWritableLocked(&backend.object,&vnode,0,PhysicalAddress{}));
    for(unsigned i=0;i<3;++i)CHECK(FileObjectExposeWritableLocked(&backend.object,&vnode,i,pages[i]));
    CHECK(vnode.refCount==2);
    KMutexUnlock(&backend.object.gate);
    auto frameCount=refs.size();unsigned allocationCount=heapBlocks;
    CHECK(FileObjectWriteback(&backend.object,&vnode,4096,8192)==0);
    CHECK(backend.writes==1&&backend.bytes[0]==0x31&&backend.bytes[4096]==0x72&&backend.bytes[8192+36]==0x31);
    unsigned before=backend.writes;
    CHECK(FileObjectWriteback(&backend.object,&vnode,8192,8192)==0&&backend.writes==before);
    CHECK(FileObjectWriteback(&backend.object,&vnode,8193,8192)==-22&&backend.writes==before);
    failHeap=true;CHECK(FileObjectWriteback(&backend.object,&vnode,0,4096)==-12);failHeap=false;
    backend.failStat=true;CHECK(FileObjectWriteback(&backend.object,&vnode,0,4096)==-5);backend.failStat=false;
    CHECK(backend.writes==before);
    // Preserve the exposed page after an error following partial progress.
    backend.writeLimit=1024;backend.errorAtWrite=backend.writes+2;
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,4096)==-5);
    CHECK(backend.bytes[0]==0x71);
    first[1500]=0x81;backend.errorAtWrite=0;
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,4096)==0&&backend.bytes[1500]==0x81);
    backend.zeroAtWrite=backend.writes+1;
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,4096)==-5);backend.zeroAtWrite=0;
    // A mapped store during backend I/O must not be lost by clearing dirty
    // state on completion. This deterministic hook runs after the snapshot.
    first[0]=0xA8;backend.redirty=first;
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,4096)==0);
    CHECK(first[0]==0xB9&&backend.bytes[0]==0xA8);
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,4096)==0&&backend.bytes[0]==0xB9);
    // EOF-tail writes never extend the file, even when the mapping modified
    // bytes past EOF inside the final resident page.
    CHECK(FileObjectWriteback(&backend.object,&vnode,8192,~uint64_t{0})==0);
    CHECK(backend.bytes.size()==8192+37&&backend.bytes.back()==0x73);
    first[0]=0xC2;
    CHECK(FileObjectWriteback(&backend.object,&vnode,1,2)==0&&backend.bytes[0]==0xC2);
    // A concurrent ordinary pwrite must not be overwritten by an older flush
    // snapshot. Hold the actual backend callback and wait until pwrite reaches
    // the gate; it can complete only after the snapshot has been written.
    backend.blockWrite=true;std::atomic<bool> ordinaryDone{false};
    std::thread flusher([&]{CHECK(FileObjectWriteback(&backend.object,&vnode,0,4096)==0);});
    {std::unique_lock<std::mutex> held(writeHookGuard);writeHookCv.wait(held,[]{return writeHookEntered;});}
    unsigned gateBefore=gateAcquires;
    std::thread ordinary([&] {
        unsigned char value=0xD3;uint64_t offset=0;
        CHECK(FileObjectWrite(&backend.object,&vnode,&value,1,&offset)==1);
        ordinaryDone=true;
    });
    while(gateAcquires==gateBefore)std::this_thread::yield();
    CHECK(!ordinaryDone);
    {std::lock_guard<std::mutex> held(writeHookGuard);writeHookReleased=true;writeHookCv.notify_all();}
    flusher.join();ordinary.join();
    CHECK(ordinaryDone&&first[0]==0xD3&&backend.bytes[0]==0xD3);
    CHECK(refs.size()==frameCount&&heapBlocks==allocationCount);
    for(auto page:pages)PmmUnrefPage(page);
    CHECK(FileObjectWritebackAll()==0&&vnode.refCount==1&&!backend.object.writebackVnode);
    FileObjectDispose(&backend.object);CHECK(refs.empty()&&heapBlocks==0);
}
static void ExtendingWriteClearsMappedTail() {
    Backend backend{};backend.bytes.resize(17,0x31);FileObjectInit(&backend.object);
    Vnode vnode{&operations,VnodeType::File,&backend,1,0};int error=0;
    auto page=FileObjectGetPage(&backend.object,&vnode,0,true,&error);CHECK(page&&!error);
    auto* bytes=reinterpret_cast<unsigned char*>(page.raw());
    bytes[80]=0xA4;bytes[150]=0xA5;
    KMutexLock(&backend.object.gate);
    CHECK(FileObjectExposeRangeLocked(&backend.object,&vnode,10,10));
    CHECK(vnode.refCount==1&&!backend.object.writebackVnode);
    CHECK(!FileObjectExposeRangeLocked(&backend.object,&vnode,11,10));
    CHECK(FileObjectExposeRangeLocked(&backend.object,&vnode,0,4096));
    KMutexUnlock(&backend.object.gate);
    unsigned char value=0x62;uint64_t position=128;
    CHECK(FileObjectWrite(&backend.object,&vnode,&value,1,&position)==1);
    CHECK(backend.bytes.size()==129&&bytes[16]==0x31&&bytes[80]==0&&bytes[128]==0x62);
    // The still-invalid tail is cleared on a subsequent partial extension.
    const unsigned char source[4]={0x73,0x74,0x75,0x76};position=200;backend.writeLimit=2;
    CHECK(FileObjectWrite(&backend.object,&vnode,source,4,&position)==2);
    CHECK(backend.bytes.size()==202&&bytes[128]==0x62&&bytes[150]==0&&bytes[200]==0x73&&bytes[201]==0x74);
    backend.writeLimit=~0u;
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,~uint64_t{0})==0);
    CHECK(backend.bytes.size()==202&&backend.bytes[80]==0&&backend.bytes[150]==0);
    backend.failStat=true;position=300;
    CHECK(FileObjectWrite(&backend.object,&vnode,&value,1,&position)==-5&&backend.bytes.size()==202);
    backend.failStat=false;
    PmmUnrefPage(page);CHECK(FileObjectWritebackAll()==0&&vnode.refCount==1);
    FileObjectDispose(&backend.object);CHECK(refs.empty()&&heapBlocks==0);
}
static void WritebackLifetime() {
    CHECK(refs.empty()&&heapBlocks==0);
    Backend first{},second{};FileObjectInit(&first.object);FileObjectInit(&second.object);
    first.bytes.resize(4096,0x21);second.bytes.resize(4096,0x32);
    Vnode a{&operations,VnodeType::File,&first,1,0},b{&operations,VnodeType::File,&second,1,0};
    int error=0;
    auto page=FileObjectGetPage(&first.object,&a,0,true,&error);CHECK(page&&!error);
    auto other=FileObjectGetPage(&second.object,&b,0,true,&error);CHECK(other&&!error);
    auto* bytes=reinterpret_cast<unsigned char*>(page.raw());bytes[0]=0x41;
    KMutexLock(&first.object.gate);
    CHECK(FileObjectExposeWritableLocked(&first.object,&a,0,page));
    CHECK(FileObjectExposeWritableLocked(&first.object,&a,0,page));
    KMutexUnlock(&first.object.gate);CHECK(a.refCount==2);
    // Model a live/retiring registry token. The actual registry's matching
    // counter transitions, including delayed ACK, run in test_vmm_leaf.
    __atomic_store_n(&first.object.mappingRegistrations,1,__ATOMIC_RELEASE);
    CHECK(FileObjectWritebackAll()==0&&a.refCount==2&&first.bytes[0]==0x41);
    bytes[0]=0x52;
    CHECK(FileObjectWritebackAll()==0&&a.refCount==2&&first.bytes[0]==0x52);
    KMutexLock(&second.object.gate);
    CHECK(FileObjectExposeWritableLocked(&second.object,&b,0,other));
    KMutexUnlock(&second.object.gate);
    reinterpret_cast<unsigned char*>(other.raw())[0]=0x63;
    // A failed object must retain backing, while another still flushes and
    // dequeues. Dropping the user descriptor leaves only the writeback pin.
    CHECK(__atomic_fetch_sub(&a.refCount,1,__ATOMIC_ACQ_REL)==2);
    first.failStat=true;
    CHECK(FileObjectWritebackAll()==-5&&a.refCount==1&&first.object.writebackVnode==&a);
    CHECK(b.refCount==1&&!second.object.writebackVnode&&second.bytes[0]==0x63);
    first.failStat=false;first.failWrite=true;
    CHECK(FileObjectWritebackAll()==-5&&a.refCount==1);
    __atomic_store_n(&first.object.mappingRegistrations,0,__ATOMIC_RELEASE);
    CHECK(FileObjectTruncateUnmapped(&first.object,&a,0)==-5&&first.bytes.size()==4096);
    CHECK(FileObjectGetPage(&first.object,&a,0,false,&error)==page);PmmUnrefPage(page);
    PmmUnrefPage(page);PmmUnrefPage(other);
    failHeap=true;CHECK(FileObjectWritebackAll()==-12&&a.refCount==1);failHeap=false;
    first.failWrite=false;unsigned closedBefore=closedVnodes;
    CHECK(FileObjectWritebackAll()==0&&first.bytes[0]==0x52&&a.refCount==0);
    CHECK(closedVnodes==closedBefore+1&&!first.object.writebackVnode);
    // Last-pin close disposed first's cache; the second user still owns theirs.
    CHECK(refs.size()==1);
    FileObjectDispose(&second.object);CHECK(refs.empty()&&heapBlocks==0);
    CHECK(FileObjectWritebackAll()==0);
}
static void BatchPages() {
    CHECK(refs.empty()&&heapBlocks==0);
    Backend backend{};FileObjectInit(&backend.object);backend.bytes.resize(16*4096,0x57);
    Vnode vnode{&operations,VnodeType::File,&backend,1,0};
    int error=0;
    auto sparse=FileObjectGetPage(&backend.object,&vnode,3,true,&error);
    CHECK(sparse&&error==0);
    auto last=FileObjectGetPage(&backend.object,&vnode,15,true,&error);
    CHECK(last&&error==0);
    PhysicalAddress pages[16]{};bool loaded=false;
    unsigned reads=backend.reads,locksBefore=gateAcquires;
    CHECK(FileObjectGetPages(&backend.object,&vnode,0,16,pages,&loaded)==0&&loaded);
    CHECK(backend.reads==reads+1&&gateAcquires==locksBefore+1);
    CHECK(pages[0]&&pages[3]==sparse&&pages[15]==last);
    for(unsigned i=1;i<16;++i)if(i!=3&&i!=15)CHECK(!pages[i]);
    CHECK(refs.at(sparse.raw())==3&&refs.at(last.raw())==3);
    auto primary=pages[0];
    for(auto page:pages)if(page)PmmUnrefPage(page);
    reads=backend.reads;locksBefore=gateAcquires;
    CHECK(FileObjectGetPages(&backend.object,&vnode,0,16,pages,&loaded)==0&&!loaded);
    CHECK(backend.reads==reads&&gateAcquires==locksBefore+1&&pages[0]==primary);
    for(auto page:pages)if(page)PmmUnrefPage(page);

    // Eight competing batches load one missing primary, retaining the same
    // sparse neighbor independently; no cache-only hole causes backend I/O.
    Barrier barrier;PhysicalAddress concurrent[8][4]{};std::vector<std::thread> workers;
    reads=backend.reads;locksBefore=gateAcquires;
    for(unsigned i=0;i<8;++i)workers.emplace_back([&,i] {
        barrier.wait();CHECK(FileObjectGetPages(&backend.object,&vnode,1,4,concurrent[i])==0);
    });
    for(auto& worker:workers)worker.join();
    CHECK(backend.reads==reads+1&&gateAcquires==locksBefore+8);
    for(auto& batch:concurrent) {
        CHECK(batch[0]==concurrent[0][0]&&!batch[1]&&batch[2]==sparse&&!batch[3]);
        for(auto page:batch)if(page)PmmUnrefPage(page);
    }
    CHECK(refs.at(sparse.raw())==2);

    // Fail before acquiring any adjacent pin; failed primaries never publish.
    const auto framesBefore=refs.size();const unsigned heapBefore=heapBlocks;
    failHeap=true;CHECK(FileObjectGetPages(&backend.object,&vnode,2,4,pages)==-12);failHeap=false;
    for(unsigned i=0;i<4;++i)CHECK(!pages[i]);
    failFrame=true;CHECK(FileObjectGetPages(&backend.object,&vnode,2,4,pages)==-12);failFrame=false;
    for(unsigned i=0;i<4;++i)CHECK(!pages[i]);
    backend.readLimit=1024;backend.zeroAtRead=backend.reads+2;
    CHECK(FileObjectGetPages(&backend.object,&vnode,2,4,pages)==-5);
    for(unsigned i=0;i<4;++i)CHECK(!pages[i]);
    backend.zeroAtRead=0;backend.errorAtRead=backend.reads+2;
    CHECK(FileObjectGetPages(&backend.object,&vnode,2,4,pages)==-5);
    for(unsigned i=0;i<4;++i)CHECK(!pages[i]);
    CHECK(refs.size()==framesBefore&&heapBlocks==heapBefore&&refs.at(sparse.raw())==2);
    CHECK(!FileObjectGetPage(&backend.object,&vnode,2,false,&error)&&error==0);
    backend.errorAtRead=0;backend.readLimit=~0u;
    CHECK(FileObjectGetPages(&backend.object,&vnode,16,4,pages)==-6);
    for(unsigned i=0;i<4;++i)CHECK(!pages[i]);
    CHECK(FileObjectGetPages(&backend.object,&vnode,~uint64_t{0}/4096,2,pages)==-22);
    CHECK(FileObjectGetPages(&backend.object,&vnode,0,0,pages)==-22);
    CHECK(FileObjectGetPages(&backend.object,&vnode,0,17,pages)==-22);

    // Caller pins remain valid after generation-changing cache eviction,
    // including a backend error after modifying its persistent data.
    CHECK(FileObjectGetPages(&backend.object,&vnode,0,16,pages)==0);
    auto generation=backend.object.generation;
    backend.failAfterTruncate=true;
    CHECK(FileObjectTruncateUnmapped(&backend.object,&vnode,1)==-5);
    CHECK(backend.object.generation==generation+1);
    CHECK(*reinterpret_cast<unsigned char*>(pages[0].raw())==0x57);
    CHECK(*reinterpret_cast<unsigned char*>(pages[3].raw())==0x57);
    CHECK(*reinterpret_cast<unsigned char*>(pages[15].raw())==0x57);
    for(auto page:pages)if(page)PmmUnrefPage(page);
    PmmUnrefPage(sparse);PmmUnrefPage(last);
    FileObjectDispose(&backend.object);CHECK(refs.empty()&&heapBlocks==0);
}

static void PartialUserCopies() {
    CHECK(refs.empty()&&heapBlocks==0);
    Backend backend{};FileObjectInit(&backend.object);backend.bytes.resize(256);
    for(unsigned i=0;i<256;++i)backend.bytes[i]=static_cast<unsigned char>(i);
    Vnode vnode{&operations,VnodeType::File,&backend,1,0};
    unsigned char buffer[128];std::memset(buffer,0xe3,sizeof(buffer));
    userBegin=reinterpret_cast<uint64_t>(buffer);userEnd=userBegin+sizeof(buffer);
    uint64_t offset=0;copyLimit=31;
    CHECK(FileObjectRead(&backend.object,&vnode,buffer,64,&offset)==31&&offset==31);
    for(unsigned i=0;i<128;++i)CHECK(buffer[i]==(i<31?i:0xe3));
    copyLimit=0;CHECK(FileObjectRead(&backend.object,&vnode,buffer,64,&offset)==-14&&offset==31);
    CHECK(FileObjectRead(&backend.object,&vnode,buffer,32,&offset)==32&&offset==63);
    for(unsigned i=0;i<32;++i)CHECK(buffer[i]==i+31);
    std::memset(buffer,0xb5,sizeof(buffer));offset=100;copyLimit=17;
    CHECK(FileObjectWrite(&backend.object,&vnode,buffer,64,&offset)==17&&offset==117);
    for(unsigned i=0;i<256;++i)CHECK(backend.bytes[i]==(i>=100&&i<117?0xb5:i));
    copyLimit=0;CHECK(FileObjectWrite(&backend.object,&vnode,buffer,64,&offset)==-14&&offset==117);
    const unsigned writes=backend.writes;CHECK(writes==1);
    // A blocked partial copyout must not reserve and lose the remainder of its
    // file bytes, or let a peer consume the same initial offset concurrently.
    offset=0;copyLimit=31;blockCopy=true;copyEntered=false;copyReleased=false;
    int results[2]={};
    std::thread first([&]{results[0]=FileObjectRead(&backend.object,&vnode,buffer,64,&offset);});
    { std::unique_lock<std::mutex> lock(copyGuard);copyCv.wait(lock,[]{return copyEntered;}); }
    const unsigned reads=backend.reads;const auto gates=gateAcquires.load();
    std::thread second([&]{results[1]=FileObjectRead(&backend.object,&vnode,buffer+64,64,&offset);});
    while(gateAcquires.load()==gates)std::this_thread::yield();
    CHECK(backend.reads==reads&&offset==0);
    { std::lock_guard<std::mutex> lock(copyGuard);copyReleased=true;copyCv.notify_all(); }
    first.join();second.join();blockCopy=false;
    CHECK(results[0]==31&&results[1]==64&&offset==95);
    for(unsigned i=0;i<31;++i)CHECK(buffer[i]==i);
    for(unsigned i=0;i<64;++i)CHECK(buffer[64+i]==31+i);
    userBegin=userEnd=0;FileObjectDispose(&backend.object);CHECK(refs.empty()&&heapBlocks==0);
}

static void QuarantinedPages() {
    CHECK(refs.empty()&&heapBlocks==0);
    Backend backend{};FileObjectInit(&backend.object);backend.bytes.resize(8192,0x57);
    Vnode vnode{&operations,VnodeType::File,&backend,1,0};int error=0;
    auto pinned=FileObjectGetPage(&backend.object,&vnode,0,true,&error);CHECK(pinned&&!error);
    KMutexLock(&backend.object.gate);
    CHECK(FileObjectExposeWritableLocked(&backend.object,&vnode,0,pinned));
    KMutexUnlock(&backend.object.gate);
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,8192)==0); // pre-attempt flush
    backend.failAfterTruncate=true;CHECK(truncate_backend(&vnode,31)==-5);
    backend.failStat=true;backend.statError=-12; // resulting EOF cannot be observed
    const auto generation=backend.object.generation;
    KMutexLock(&backend.object.gate);
    FileObjectQuarantineLocked(&backend.object,-5,8192);
    CHECK(backend.object.mappingError==-5&&backend.object.privateValidPrefix==8192);
    FileObjectQuarantineLocked(&backend.object,-5,4097);
    FileObjectQuarantineLocked(&backend.object,-5,9000);
    CHECK(backend.object.privateValidPrefix==4097&&backend.object.generation==generation+3);
    CHECK(FileObjectIsCanonicalLocked(&backend.object,0,pinned)); // cache retained, inaccessible
    CHECK(!FileObjectExposeWritableLocked(&backend.object,&vnode,0,pinned));
    KMutexUnlock(&backend.object.gate);
    const auto bytes=backend.bytes;const auto reads=backend.reads,writes=backend.writes;
    bool loaded=true;PhysicalAddress pages[4];
    CHECK(!FileObjectGetPage(&backend.object,&vnode,0,true,&error,&loaded)&&error==-5&&!loaded);
    CHECK(!FileObjectGetPage(&backend.object,&vnode,0,false,&error)&&error==-5);
    CHECK(FileObjectGetPages(&backend.object,&vnode,0,4,pages)==-5);for(auto page:pages)CHECK(!page);
    CHECK(FileObjectFaultPages(&backend.object,&vnode,0,4,pages)==MappingFaultResult::IoError);
    for(auto page:pages)CHECK(!page);
    unsigned char buffer[64]{};uint64_t offset=19;
    CHECK(FileObjectRead(&backend.object,&vnode,buffer,sizeof(buffer),&offset)==-5&&offset==19);
    CHECK(FileObjectWrite(&backend.object,&vnode,buffer,sizeof(buffer),&offset)==-5&&offset==19);
    CHECK(FileObjectAppend(&backend.object,&vnode,buffer,sizeof(buffer),&offset)==-5&&offset==19);
    CHECK(FileObjectTruncateUnmapped(&backend.object,&vnode,9000)==-5);
    CHECK(FileObjectWriteback(&backend.object,&vnode,0,8192)==-5);
    CHECK(FileObjectRead(&backend.object,&vnode,buffer,0,&offset)==0);
    CHECK(FileObjectWrite(&backend.object,&vnode,buffer,0,&offset)==0);
    CHECK(FileObjectAppend(&backend.object,&vnode,buffer,0,&offset)==0);
    backend.object.mappingRegistrations=1;
    CHECK(FileObjectWritebackAll()==-5&&backend.object.writebackVnode==&vnode&&vnode.refCount==2);
    backend.object.mappingRegistrations=0;
    CHECK(FileObjectWritebackAll()==-5&&!backend.object.writebackVnode&&vnode.refCount==1);
    CHECK(FileObjectWritebackAll()==0); // finite queue retirement, no orphan cycle
    CHECK(backend.object.mappingError==-5&&backend.object.privateValidPrefix==4097);
    CHECK(backend.bytes==bytes&&backend.reads==reads&&backend.writes==writes);
    CHECK(refs.size()==1&&refs.at(pinned.raw())==1); // external pin survives cache eviction
    CHECK(*reinterpret_cast<unsigned char*>(pinned.raw())==0x57);
    backend.failStat=false;backend.failAfterTruncate=false;
    VnodeStat observed{};CHECK(stat_backend(&vnode,&observed)==0&&observed.size==31);
    KMutexLock(&backend.object.gate);FileObjectCompleteMappingRecoveryLocked(&backend.object);KMutexUnlock(&backend.object.gate);
    CHECK(!backend.object.mappingError&&backend.object.privateValidPrefix==~uint64_t{0});
    auto fresh=FileObjectGetPage(&backend.object,&vnode,0,true,&error);CHECK(fresh&&fresh!=pinned&&!error);
    auto* data=reinterpret_cast<unsigned char*>(fresh.raw());CHECK(data[0]==0xA7&&data[30]==0x57&&data[31]==0);
    offset=31;CHECK(FileObjectAppend(&backend.object,&vnode,buffer,1,&offset)==1&&offset==32);
    PmmUnrefPage(fresh);PmmUnrefPage(pinned);FileObjectDispose(&backend.object);CHECK(refs.empty()&&heapBlocks==0);
}

static void FaultPages() {
    CHECK(refs.empty() && heapBlocks==0);
    Backend backend{};FileObjectInit(&backend.object);backend.bytes.resize(4097,0x5a);
    Vnode vnode{&operations,VnodeType::File,&backend,1,0};
    PhysicalAddress pages[4];bool loaded=true;
    CHECK(FileObjectFaultPages(&backend.object,&vnode,2,4,pages,&loaded)==MappingFaultResult::EndOfFile);
    CHECK(!loaded && backend.reads==0 && refs.empty());for(auto page:pages)CHECK(!page);
    backend.failStat=true;
    CHECK(FileObjectFaultPages(&backend.object,&vnode,0,4,pages)==MappingFaultResult::IoError);
    backend.statError=-12;
    CHECK(FileObjectFaultPages(&backend.object,&vnode,0,4,pages)==MappingFaultResult::OutOfMemory);
    backend.statError=-5;
    backend.failStat=false;failHeap=true;
    CHECK(FileObjectFaultPages(&backend.object,&vnode,0,4,pages)==MappingFaultResult::OutOfMemory);
    failHeap=false;backend.failRead=true;
    CHECK(FileObjectFaultPages(&backend.object,&vnode,0,4,pages)==MappingFaultResult::IoError);
    backend.readError=-6;
    CHECK(FileObjectFaultPages(&backend.object,&vnode,0,4,pages)==MappingFaultResult::IoError);
    backend.readError=-12;
    CHECK(FileObjectFaultPages(&backend.object,&vnode,0,4,pages)==MappingFaultResult::OutOfMemory);
    backend.readError=-5;
    backend.failRead=false;CHECK(refs.empty() && heapBlocks==0);
    CHECK(FileObjectFaultPages(&backend.object,&vnode,1,4,pages,&loaded)==MappingFaultResult::Resolved);
    CHECK(loaded && pages[0] && !pages[1] && !pages[2] && !pages[3]);
    auto* tail=reinterpret_cast<unsigned char*>(pages[0].raw());
    CHECK(tail[0]==0x5a && tail[1]==0 && tail[4095]==0);PmmUnrefPage(pages[0]);
    CHECK(FileObjectFaultPages(&backend.object,&vnode,0,4,pages)==MappingFaultResult::Resolved);
    CHECK(pages[0] && pages[1] && !pages[2] && !pages[3]);
    for(auto page:pages)if(page)PmmUnrefPage(page);
    CHECK(FileObjectFaultPages(&backend.object,&vnode,~uint64_t{0}/4096+1,4,pages)==MappingFaultResult::EndOfFile);
    for(auto page:pages)CHECK(!page);
    FileObjectDispose(&backend.object);CHECK(refs.empty() && heapBlocks==0);
    std::puts("FILE FAULT PREP PASS EOF/IO/OOM/partial-tail/bounded-neighbors/reference-cleanup");
}
int main() {
    Backend backend{};FileObjectInit(&backend.object);backend.bytes.resize(8192,0x31);
    Vnode first{&operations,VnodeType::File,&backend,1,0},second=first;
    Barrier barrier;PhysicalAddress pages[8];std::vector<std::thread> workers;
    for(unsigned i=0;i<8;++i)workers.emplace_back([&,i]{barrier.wait();int error=0;pages[i]=FileObjectGetPage(&backend.object,i&1?&first:&second,0,true,&error);CHECK(pages[i]&&error==0);});
    for(auto& worker:workers)worker.join();
    for(auto page:pages)CHECK(page==pages[0]);CHECK(backend.reads==1);
    auto* bytes=reinterpret_cast<unsigned char*>(pages[0].raw());CHECK(bytes[17]==0x31);
    unsigned char change=0x72;uint64_t offset=17;
    CHECK(FileObjectWrite(&backend.object,&second,&change,1,&offset)==1&&offset==18&&bytes[17]==change);
    // A simulated shared store must overlay ordinary backend reads. Writable
    // mappings remain disabled in the kernel until writeback/resize land.
    bytes[21]=0x84;unsigned char result=0;offset=21;
    CHECK(FileObjectRead(&backend.object,&first,&result,1,&offset)==1&&result==0x84&&offset==22);
    unsigned char update[2]={0x66,0x67};backend.writeLimit=1;offset=30;
    CHECK(FileObjectWrite(&backend.object,&second,update,2,&offset)==1&&offset==31);
    CHECK(bytes[30]==0x66&&bytes[31]==0x31);
    backend.failWrite=true;offset=30;
    CHECK(FileObjectWrite(&backend.object,&first,update,2,&offset)==-5&&offset==30&&bytes[30]==0x66);
    backend.failWrite=false;
    for(auto page:pages)PmmUnrefPage(page);
    uint64_t generation=backend.object.generation;
    backend.failTruncate=true;CHECK(FileObjectTruncateUnmapped(&backend.object,&first,0)==-5);
    CHECK(backend.object.generation==generation+1);
    int error=0;CHECK(!FileObjectGetPage(&backend.object,&second,0,false,&error)&&error==0);
    backend.failTruncate=false;
    auto reloaded=FileObjectGetPage(&backend.object,&second,0,true,&error);CHECK(reloaded&&error==0);
    CHECK(reinterpret_cast<unsigned char*>(reloaded.raw())[0]==0x31);PmmUnrefPage(reloaded);
    generation=backend.object.generation;backend.failAfterTruncate=true;
    CHECK(FileObjectTruncateUnmapped(&backend.object,&first,31)==-5);
    CHECK(backend.object.generation==generation+1&&backend.bytes.size()==31);
    CHECK(!FileObjectGetPage(&backend.object,&second,0,false,&error)&&error==0);
    auto changed=FileObjectGetPage(&backend.object,&second,0,true,&error);CHECK(changed&&error==0);
    auto* changedBytes=reinterpret_cast<unsigned char*>(changed.raw());
    CHECK(changedBytes[0]==0xA7&&changedBytes[30]==0x66&&changedBytes[31]==0&&changedBytes[4095]==0);
    PmmUnrefPage(changed);backend.failAfterTruncate=false;
    CHECK(FileObjectTruncateUnmapped(&backend.object,&second,7)==0);
    CHECK(!FileObjectGetPage(&backend.object,&first,0,false,&error));
    auto shortPage=FileObjectGetPage(&backend.object,&second,0,true,&error);CHECK(shortPage);
    auto* tail=reinterpret_cast<unsigned char*>(shortPage.raw());CHECK(tail[6]==0x31&&tail[7]==0&&tail[4095]==0);PmmUnrefPage(shortPage);
    CHECK(!FileObjectGetPage(&backend.object,&first,1,true,&error)&&error==-6);
    CHECK(FileObjectTruncateUnmapped(&backend.object,&first,8192)==0);
    backend.failRead=true;CHECK(!FileObjectGetPage(&backend.object,&first,1,true,&error)&&error==-5);
    backend.failRead=false;backend.readLimit=1024;backend.zeroAtRead=backend.reads+2;
    CHECK(!FileObjectGetPage(&backend.object,&first,1,true,&error)&&error==-5);
    CHECK(!FileObjectGetPage(&backend.object,&second,1,false,&error)&&error==0);
    backend.zeroAtRead=0;backend.errorAtRead=backend.reads+2;
    CHECK(!FileObjectGetPage(&backend.object,&first,1,true,&error)&&error==-5);
    CHECK(!FileObjectGetPage(&backend.object,&second,1,false,&error)&&error==0);
    backend.errorAtRead=0;unsigned before=backend.reads;bool loaded=false;
    auto retry=FileObjectGetPage(&backend.object,&second,1,true,&error,&loaded);CHECK(retry&&error==0&&loaded);
    CHECK(backend.reads-before==4);
    CHECK(*reinterpret_cast<unsigned char*>(retry.raw())==0);PmmUnrefPage(retry);
    FileObjectDispose(&backend.object);CHECK(refs.empty()&&heapBlocks==0);
    BatchPages();
    WritebackPages();
    WritebackLifetime();
    ExtendingWriteClearsMappedTail();
    AppendTransactions();
    FaultPages();
    QuarantinedPages();
    PartialUserCopies();
    std::puts("FILE_OBJECT PASS batch/sparse-pins/one-gate/failure-cleanup/concurrent-load/io-coherence/partial-error/truncate/zero-tail/lifetime/writeback-range/snapshot/redirty/error-retry");
}

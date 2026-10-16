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
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::abort(); } } while (0)
using namespace brook;
namespace {
std::mutex locksGuard,framesGuard;
std::unordered_map<KMutex*,std::unique_ptr<std::mutex>> locks;
std::unordered_map<uint64_t,unsigned> refs;
std::atomic<unsigned> heapBlocks{0};
struct Backend {
    FileObject object;
    std::vector<unsigned char> bytes;
    unsigned reads=0;
    bool failRead=false,failWrite=false,failTruncate=false,failAfterTruncate=false;
    unsigned writeLimit=~0u,readLimit=~0u,zeroAtRead=0,errorAtRead=0;
};
int read_backend(Vnode* vnode,void* buffer,uint64_t length,uint64_t* offset) {
    auto* backend=static_cast<Backend*>(vnode->priv);++backend->reads;
    if(backend->failRead || backend->reads==backend->errorAtRead)return -5;
    if(backend->reads==backend->zeroAtRead)return 0;
    if(*offset>=backend->bytes.size())return 0;
    size_t count=backend->bytes.size()-*offset;if(count>length)count=length;
    if(count>backend->readLimit)count=backend->readLimit;
    std::memcpy(buffer,backend->bytes.data()+*offset,count);*offset+=count;return static_cast<int>(count);
}
int write_backend(Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset) {
    auto* backend=static_cast<Backend*>(vnode->priv);if(backend->failWrite)return -5;
    if(length>backend->writeLimit)length=backend->writeLimit;
    if(*offset+length>backend->bytes.size())backend->bytes.resize(*offset+length);
    std::memcpy(backend->bytes.data()+*offset,buffer,length);*offset+=length;return static_cast<int>(length);
}
int stat_backend(Vnode* vnode,VnodeStat* stat) { stat->size=static_cast<Backend*>(vnode->priv)->bytes.size();return 0; }
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
const VnodeOps operations={nullptr,read_backend,write_backend,nullptr,nullptr,stat_backend,nullptr,nullptr,nullptr,truncate_backend};
struct Barrier {
    std::mutex guard;std::condition_variable cv;unsigned arrived=0;
    void wait() { std::unique_lock<std::mutex> lock(guard);++arrived;cv.notify_all();cv.wait(lock,[&]{return arrived==8;}); }
};
}
namespace brook {
void KMutexInit(KMutex* mutex) { std::lock_guard<std::mutex> lock(locksGuard);locks[mutex]=std::make_unique<std::mutex>(); }
void KMutexLock(KMutex* mutex) { std::mutex* lock;{std::lock_guard<std::mutex> held(locksGuard);lock=locks.at(mutex).get();}lock->lock(); }
void KMutexUnlock(KMutex* mutex) { std::mutex* lock;{std::lock_guard<std::mutex> held(locksGuard);lock=locks.at(mutex).get();}lock->unlock(); }
extern "C" void* kmalloc(uint64_t bytes) { auto* memory=std::malloc(bytes);if(memory)++heapBlocks;return memory; }
extern "C" void kfree(void* memory) { if(memory){CHECK(heapBlocks.fetch_sub(1)>0);std::free(memory);} }
PhysicalAddress PmmAllocPage(MemTag,uint16_t) {
    void* memory=nullptr;CHECK(posix_memalign(&memory,4096,4096)==0);
    auto address=reinterpret_cast<uint64_t>(memory);std::lock_guard<std::mutex> lock(framesGuard);refs[address]=1;return PhysicalAddress(address);
}
void PmmRefPage(PhysicalAddress page) { std::lock_guard<std::mutex> lock(framesGuard);CHECK(refs.at(page.raw())>0);++refs.at(page.raw()); }
void PmmUnrefPage(PhysicalAddress page) { std::lock_guard<std::mutex> lock(framesGuard);CHECK(refs.at(page.raw())>0);if(!--refs.at(page.raw())){refs.erase(page.raw());std::free(reinterpret_cast<void*>(page.raw()));} }
}
extern "C" __attribute__((noreturn)) void KernelPanic(const char*,...) { std::abort(); }
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
    std::puts("FILE_OBJECT PASS concurrent-load/io-coherence/partial-error/truncate/zero-tail/lifetime");
}

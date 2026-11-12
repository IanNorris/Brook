#include "intel_bdw_buffer.h"
#include "intel_bdw_bootstrap_gate.h"
#include "intel_bdw_file.h"
#include "intel_bdw_fence.h"
#include "intel_bdw_handoff.h"
#include "intel_bdw_scanout_lifetime.h"
#include "intel_bdw_scanout_mapping.h"
#include "intel_bdw_ggtt_extent_mmio.h"
#include "intel_bdw_handoff_session.h"
#include "sync_file.h"
#include "intel_bdw_ioctl.h"
#include "intel_bdw_exec_snapshot.h"
#include "intel_bdw_exec_queue.h"
#include "intel_bdw_submission_queue.h"
#include "intel_bdw_submission_worker.h"
#include "intel_bdw_render_oneshot.h"
#include "intel_bdw_exec_tables.h"
#include "intel_bdw_scanout.h"
#include "intel_vtd_observe.h"
#include "intel_bdw_exec_address_space.h"
#include "memory/mapping_ranges.h"
#include "memory/virtual_memory.h"
#include "sync/kmutex.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
using namespace brook;
struct Allocation {void* ptr;uint64_t pages,dma;};
static Allocation allocations[16]{};
static unsigned frees=0,allocated=0,failAt=0;
static bool failVmm=false,failBacking=false,badDma=false;
static bool resetArmed=true,translated=false,wrongOwner=false,changingVtd=false;
static unsigned vtdReads=0;
struct FakeUser {bool readable=true,writable=true,writeOk=true;unsigned reads=0,failRead=0;
    uint64_t now=0;unsigned sleeps=0,completeAt=0;bool interrupt=false;BdwGpuBuffer* waiting=nullptr;
    BdwExecQueue* queue=nullptr;uint64_t request=0;bool rejectSubmission=false;
    BdwGpuSync* transferSync=nullptr;BdwFence* transferFence=nullptr;
    BdwSubmissionWorker* handoffWorker=nullptr;BdwHandoffResources* handoff=nullptr;BdwGpuBuffer* handoffBuffer=nullptr;
    HandoffTicket handoffTicket{};uint64_t handoffOwner=0,handoffToken=0;unsigned handoffCalls=0;
    BdwHandoffSession* session=nullptr;};
static bool ReadUser(void* c,void* dst,uint64_t src,uint64_t n){auto* u=static_cast<FakeUser*>(c);++u->reads;if(!u->readable || !src || (u->failRead && u->reads==u->failRead))return false;std::memcpy(dst,reinterpret_cast<void*>(src),n);return true;}
static bool WritableUser(void* c,uint64_t dst,uint64_t){return static_cast<FakeUser*>(c)->writable && dst;}
static bool WriteUser(void* c,uint64_t dst,const void* src,uint64_t n){auto* u=static_cast<FakeUser*>(c);if(!u->writeOk || !dst)return false;std::memcpy(reinterpret_cast<void*>(dst),src,n);return true;}
static uint64_t UserNow(void* c){return static_cast<FakeUser*>(c)->now;}
static bool UserSleep(void* c,uint64_t n){auto* u=static_cast<FakeUser*>(c);u->now+=n;++u->sleeps;
    if(u->waiting && u->sleeps==u->completeAt)assert(BdwGpuBufferEndGpuUse(u->waiting));
    if(u->transferSync && u->sleeps==u->completeAt)assert(BdwIntelSyncReplaceAdmitted(u->transferSync,u->transferFence));return !u->interrupt;}
static int64_t UserSubmit(void* c,BdwExecSnapshot* snapshot){auto* u=static_cast<FakeUser*>(c);
    return u->queue && !u->rejectSubmission && u->queue->Enqueue(9,snapshot,u->request)?0:-16;}
struct FakeWorker {
    void (*entry)(void*)=nullptr;void* argument=nullptr;uint32_t cpu=~uint32_t{0};
    bool launchOk=true,safelyStopped=true,success=true;unsigned executions=0;
    static bool Launch(void* c,void (*entry)(void*),void* arg,uint32_t cpu){auto& f=*static_cast<FakeWorker*>(c);
        f.entry=entry;f.argument=arg;f.cpu=cpu;return f.launchOk;}
    static void Sleep(void*){}
    static BdwSubmissionOutcome Execute(void* c,BdwExecSnapshot*){auto& f=*static_cast<FakeWorker*>(c);++f.executions;return {f.success,f.safelyStopped};}
    static void Recover(void*){assert(false);}
    BdwSubmissionWorker::Hooks Hooks(){return {this,Launch,Sleep,Execute,Recover};}
};
static bool UserHandoffSubmit(void* context,BdwExecSnapshot* snapshot,int64_t* result){
    auto* u=static_cast<FakeUser*>(context);
    if(!u->handoffWorker || !u->handoff || !u->handoffBuffer)return false;
    if(!BdwExecSnapshotContainsBuffer(snapshot,u->handoffBuffer))return false;
    auto* fence=BdwExecSnapshotAcquireWriteSignal(snapshot,u->handoffBuffer);
    if(!fence){++u->handoffCalls;*result=-22;return true;}
    ++u->handoffCalls;*result=u->handoffWorker->SubmitHandoff(*u->handoff,u->handoffOwner,u->handoffTicket,snapshot,fence,u->handoffToken);
    BdwFenceUnref(fence);return true;
}
static bool UserSessionSubmit(void* context,BdwExecSnapshot* snapshot,int64_t* result){
    auto* u=static_cast<FakeUser*>(context);++u->handoffCalls;
    return u->session->Submit(u->handoffOwner,*u->handoffWorker,snapshot,*result);
}
static uint64_t ioctlCommand(uint32_t nr,uint32_t size){return 0xc0000000u|(uint64_t(size)<<16)|('d'<<8)|nr;}
struct RenderAllocator {
    unsigned allocated=0,freed=0;bool publishOk=true;
    bool alloc(BdwOwnedPage& page){void* p=nullptr;if(posix_memalign(&p,4096,4096))return false;
        page={0x40000000ull+uint64_t(allocated++)*4096,static_cast<uint8_t*>(p)};return true;}
    void free(BdwOwnedPage page){std::free(page.cpu);++freed;}
    bool publish(BdwOwnedPage*,unsigned n){return n==37 && publishOk;}
};
struct TableAllocator {
    unsigned allocated=0,freed=0,failAt=0;bool publishOk=true;uint32_t dmaStatus=0;
    bool alloc(BdwOwnedPage& page){if(failAt && allocated+1==failAt)return false;void* p=nullptr;
        if(posix_memalign(&p,4096,4096))return false;page={0x60000000ull+uint64_t(allocated++)*4096,static_cast<uint8_t*>(p)};return true;}
    void free(BdwOwnedPage p){std::free(p.cpu);++freed;}
    uint32_t inspectDma(const BdwOwnedPage*,unsigned count){return count?dmaStatus:1;}
    bool publish(BdwOwnedPage*,unsigned count){return count && publishOk;}
};
struct RenderBackend {
    BdwOwnedPage* pages=nullptr;uint64_t ptes[29]{};
    bool exclusiveHandoff(){return true;} bool renderStateReady(){return true;}
    bool admitPages(BdwOwnedPage* p,unsigned n){pages=p;return n==37;}
    bool admitExtraTables(BdwOwnedPage*,unsigned n){return n>=4 && n<=128;}bool publishExtraTables(BdwOwnedPage*,unsigned n){return n>=4 && n<=128;}
    bool gpuPatLease(){return true;} bool preparePrivatePat(uint64_t){return true;}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi){lo=7;hi=11;return true;}
    bool publish(BdwOwnedPage* p,unsigned n){return p==pages && n==37;}
    bool validateLease(const BdwGgttLease& l){return l.token==42 && l.count==29;}
    bool validateTakeover(const BdwGgttLease& l,uint64_t scratch){return validateLease(l) && scratch==0x800000;}
    bool quiescent(){return true;}
    bool read(uint32_t i,uint64_t& v){if(i<65536 || i>=65565)return false;v=ptes[i-65536];return true;}
    bool write(uint32_t i,uint64_t v){if(i<65536 || i>=65565)return false;ptes[i-65536]=v;return true;}
    bool invalidate(){return true;}
    bool initializeExeclists(uint64_t,const BdwOwnedPage&){return true;}
    bool prepareSubmission(uint64_t){return true;} bool submitDword(uint32_t){return true;}
    bool waitCompletion(uint32_t nonce){std::memcpy(pages[25].cpu,&nonce,sizeof(nonce));return true;}
    bool cpuVisible(BdwOwnedPage* p,unsigned n){return p==pages && n==37;}
    bool csbReadTail(BdwOwnedPage&,uint32_t& tail){tail=1;return true;}
    bool csbReadEntry(BdwOwnedPage&,unsigned slot,uint64_t& event){event=slot?((uint64_t{1}<<32)|0x18):1;return true;}
    bool csbAcknowledge(unsigned){return true;}
};
static uint32_t renderStaging[20*1024];
alignas(4096) static uint8_t pageTables[8][4096];
namespace brook {
static bool failHeapOnce=false;
extern "C" void* kmalloc(uint64_t n){if(failHeapOnce){failHeapOnce=false;return nullptr;}return std::malloc(n);}
extern "C" void kfree(void* p){std::free(p);}
VirtualAddress VmmAllocPages(uint64_t n,uint64_t,MemTag,uint16_t){
    if(failVmm || (failAt && allocated+1==failAt))return VirtualAddress{};
    assert(allocated<16 && n<=256);auto& a=allocations[allocated];
    assert(!posix_memalign(&a.ptr,4096,n*4096));a.pages=n;a.dma=0x100000+uint64_t(allocated)*0x200000;
    ++allocated;std::memset(a.ptr,0xaa,n*4096);return VirtualAddress(reinterpret_cast<uint64_t>(a.ptr));
}
void VmmFreePages(VirtualAddress address,uint64_t n){
    for(unsigned i=0;i<allocated;++i)if(address.raw()==reinterpret_cast<uint64_t>(allocations[i].ptr)){
        assert(n==allocations[i].pages);std::free(allocations[i].ptr);allocations[i].ptr=nullptr;++frees;return;}
    assert(false);
}
PhysicalAddress VmmVirtToPhys(PageTable,VirtualAddress address){
    for(unsigned i=0;i<allocated;++i){const auto& a=allocations[i];const uint64_t base=reinterpret_cast<uint64_t>(a.ptr);
        if(a.ptr && address.raw()>=base && address.raw()-base<a.pages*4096){const uint64_t offset=address.raw()-base;
            return PhysicalAddress(badDma && offset==4096 ? (uint64_t{16}<<30) : a.dma+offset);}}
    return PhysicalAddress{};
}
MappingBacking* MappingBackingCreate(void* object,void (*release)(void*)){
    if(failBacking)return nullptr;
    return new MappingBacking{1,object,release};
}
void MappingBackingRef(MappingBacking* backing){assert(backing && backing->references);++backing->references;}
void MappingBackingUnref(MappingBacking* backing){assert(backing && backing->references);if(!--backing->references){backing->release(backing->object);delete backing;}}
void KMutexInit(KMutex* m){*m={};}
void KMutexLock(KMutex* m){uint32_t expected=0;while(!__atomic_compare_exchange_n(&m->locked,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){expected=0;std::this_thread::yield();}}
void KMutexUnlock(KMutex* m){assert(__atomic_load_n(&m->locked,__ATOMIC_RELAXED));__atomic_store_n(&m->locked,0,__ATOMIC_RELEASE);}
bool PanicRecoveryResetArmed(){return resetArmed;}
uint32_t SmpCurrentCpuIndex(){return 0;}
MemTag PmmGetTag(PhysicalAddress){return wrongOwner?MemTag::User:MemTag::Device;}
uint16_t PmmGetPid(PhysicalAddress){return KernelPid;}
const DellGpuVtdObservation& DellGpuVtdObserveRead(){
    static DellGpuVtdObservation o{};o.sampled=o.topology=o.inventory=o.bootMap=true;return o;
}
bool VmmDellGpuVtdCriticalRead(DellGpuVtdStatus& s){++vtdReads;s={0x10,translated?0x80000000u:0u,32,
    changingVtd && !(vtdReads&1)?2u:0u,true};return true;}
}
int main(){
    {
        BdwTimingObjectHints hints{};
        BdwTimingCountObjectHints(hints,0,3);
        BdwTimingCountObjectHints(hints,4,5);
        BdwTimingCountObjectHints(hints,64,7);
        BdwTimingCountObjectHints(hints,4|64,11);
        assert(hints.explicitWritePages==16 && hints.asyncPages==18);
        struct FakeBuffer {uint32_t pages;};
        FakeBuffer a{3},b{7},sameSizeDifferentObject{3};
        FakeBuffer* buffers[]={&a,&b,&a,&sameSizeDifferentObject,&b};
        uint64_t unique=0,repeated=0;
        BdwTimingCountBufferPages(buffers,5,[](FakeBuffer* item){return item->pages;},unique,repeated);
        assert(unique==13 && repeated==10);
        BdwTimingCountBufferPages(buffers,0,[](FakeBuffer* item){return item->pages;},unique,repeated);
        assert(unique==0 && repeated==0);
        BdwTimingSnapshot t{};t.owner=42;
        BdwTimingAccumulate(t,43,BdwTimingQueue,50,2);
        BdwTimingAccumulate(t,42,BdwTimingQueue,10,2);
        BdwTimingAccumulate(t,42,BdwTimingQueue,20,3);
        BdwTimingAccumulate(t,42,BdwTimingDevice,7,1);
        BdwTimingAccumulate(t,42,BdwTimingAdmissionWait,11,0);
        BdwTimingAccumulate(t,42,BdwTimingSnapshotBuild,17,0);
        BdwTimingAccumulate(t,42,BdwTimingStageCount,99,9);
        assert(t.stages[BdwTimingQueue].count==2 && t.stages[BdwTimingQueue].totalUs==30);
        assert(t.stages[BdwTimingQueue].maxUs==20 && t.stages[BdwTimingQueue].totalPages==5);
        assert(t.stages[BdwTimingDevice].count==1 && !t.saturated);
        assert(t.stages[BdwTimingAdmissionWait].totalUs==11 && t.stages[BdwTimingSnapshotBuild].totalUs==17);
        t.stages[BdwTimingQueue].totalUs=~uint64_t{0};
        BdwTimingAccumulate(t,42,BdwTimingQueue,1,0);
        assert(t.saturated && t.stages[BdwTimingQueue].count==2);
    }
    {
        SyncFileData completed{1,123,16};SyncFileInfo info{};SyncFenceInfo detail{};
        assert(SyncFileReady(completed,1)==1);SyncFileDescribe(completed,info,detail);
        assert(info.status==1 && detail.status==1 && detail.timestampNs==123);
        auto* fence=BdwFenceCreate();assert(fence);
        SyncFileData file{1,0,17,fence};
        assert(!SyncFileReady(file,1));SyncFileDescribe(file,info,detail);
        assert(info.status==0 && detail.status==0 && info.numFences==1 && !detail.timestampNs);
        assert(BdwFenceComplete(fence,true) && SyncFileReady(file,1)==1 && !SyncFileReady(file,4));
        SyncFileDescribe(file,info,detail);assert(info.status==1 && detail.status==1 && !detail.timestampNs);
        BdwFenceUnref(fence);
        fence=BdwFenceCreate();assert(fence && BdwFenceComplete(fence,false));file.intelFence=fence;
        assert(SyncFileReady(file,1)==1);SyncFileDescribe(file,info,detail);
        assert(info.status==-5 && detail.status==-5);BdwFenceUnref(fence);
    }
    {
        auto* file=BdwGpuFileCreate();assert(file);const auto id=BdwGpuFileCreateSync(file,false);assert(id);
        auto* sync=BdwGpuFileGetSync(file,id);auto* old=BdwFenceCreate();auto* newer=BdwFenceCreate();
        assert(sync && old && newer && BdwIntelSyncReplaceAdmitted(sync,old));
        auto* exported=BdwGpuSyncAcquire(sync);assert(exported==old);
        SyncFileData snapshot{1,0,18,exported};
        assert(BdwIntelSyncReplaceAdmitted(sync,newer) && BdwFenceComplete(newer,true));
        assert(!SyncFileReady(snapshot,1));
        assert(BdwGpuFileDestroySync(file,id));BdwGpuSyncUnref(sync);BdwGpuFileUnref(file);
        assert(!SyncFileReady(snapshot,1) && BdwFenceComplete(old,true) && SyncFileReady(snapshot,1)==1);
        BdwFenceUnref(exported);BdwFenceUnref(old);BdwFenceUnref(newer);
    }
    {
        BdwBootstrapGate gate;std::atomic<unsigned> owners{0};std::thread contenders[8];
        for(auto& contender:contenders)contender=std::thread([&]{if(gate.TryStart())++owners;});
        for(auto& contender:contenders)contender.join();
        assert(owners==1 && gate.State()==1 && !gate.TryStart());
        gate.Finish(false);assert(gate.State()==3 && !gate.TryStart());
        BdwBootstrapGate ready;assert(ready.TryStart());ready.Finish(true);
        assert(ready.State()==2 && !ready.TryStart());
    }
    {
        auto* file=BdwGpuFileCreate();auto* other=BdwGpuFileCreate();assert(file && other);
        FakeUser fake;BdwI915UserAccess user{&fake,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        struct Sync {uint32_t handle,flags;} a{},b{0,1};
        struct Array {uint64_t handles;uint32_t count,pad;};
        struct Wait {uint64_t handles;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
        const auto create=ioctlCommand(0xbf,sizeof(a)),destroy=ioctlCommand(0xc0,sizeof(a));
        const auto signal=ioctlCommand(0xc5,sizeof(Array)),reset=ioctlCommand(0xc4,sizeof(Array));
        const auto waitCmd=ioctlCommand(0xc3,sizeof(Wait));
        fake.writeOk=false;for(unsigned i=0;i<260;++i)assert(BdwIntelRenderIoctl(file,create,reinterpret_cast<uint64_t>(&a),user)==-14);
        fake.writeOk=true;assert(!BdwIntelRenderIoctl(file,create,reinterpret_cast<uint64_t>(&a),user) && a.handle);
        assert(!BdwIntelRenderIoctl(file,create,reinterpret_cast<uint64_t>(&b),user) && b.handle && a.handle!=b.handle);
        assert(!BdwGpuFileGetSync(other,a.handle));
        uint32_t handles[]={a.handle,b.handle};Array list{reinterpret_cast<uint64_t>(handles),2,0};
        Wait w{reinterpret_cast<uint64_t>(handles),0,2,0,99,0,0};
        Wait empty{0,0,0,0,77,0,0};
        assert(!BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&empty),user) && empty.first==77);
        empty.flags=4;assert(BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&empty),user)==-22);
        w.flags=4;assert(BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&w),user)==-22);w.flags=0;
        assert(BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&w),user)==-22);
        w.flags=2;assert(!BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&w),user) && w.first==1);
        w.flags=3;w.timeout=1000000;assert(BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&w),user)==-62);
        assert(!BdwIntelRenderIoctl(file,signal,reinterpret_cast<uint64_t>(&list),user));
        w.flags=1;w.timeout=0;assert(!BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&w),user));
        assert(!BdwIntelRenderIoctl(file,reset,reinterpret_cast<uint64_t>(&list),user));
        w.flags=2;assert(BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&w),user)==-62);
        const auto invalid=handles[1];handles[1]=999999;
        assert(BdwIntelRenderIoctl(file,signal,reinterpret_cast<uint64_t>(&list),user)==-2);handles[1]=invalid;
        w.flags=2;assert(BdwIntelRenderIoctl(file,waitCmd,reinterpret_cast<uint64_t>(&w),user)==-62);
        a.flags=0;b.flags=0;assert(!BdwIntelRenderIoctl(file,destroy,reinterpret_cast<uint64_t>(&a),user));
        assert(!BdwIntelRenderIoctl(file,destroy,reinterpret_cast<uint64_t>(&b),user));
        BdwGpuFileUnref(file);BdwGpuFileUnref(other);
    }
    {
        auto* file=BdwGpuFileCreate();auto* other=BdwGpuFileCreate();assert(file && other);
        FakeUser fake;BdwI915UserAccess user{&fake,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        struct CreateContext {uint32_t id,flags;} create{};
        struct Param {uint32_t id,size;uint64_t param,value;} param{};
        const auto createCmd=ioctlCommand(0x6d,sizeof(create)),destroyCmd=ioctlCommand(0x6e,sizeof(create));
        const auto getCmd=ioctlCommand(0x74,sizeof(param)),setCmd=ioctlCommand(0x75,sizeof(param));
        param.param=3;assert(!BdwIntelRenderIoctl(file,getCmd,reinterpret_cast<uint64_t>(&param),user) && param.value==(uint64_t{1}<<48));
        fake.writeOk=false;
        for(unsigned i=0;i<70;++i)assert(BdwIntelRenderIoctl(file,createCmd,reinterpret_cast<uint64_t>(&create),user)==-14);
        fake.writeOk=true;assert(!BdwIntelRenderIoctl(file,createCmd,reinterpret_cast<uint64_t>(&create),user) && create.id);
        param={create.id,0,8,0};assert(!BdwIntelRenderIoctl(file,setCmd,reinterpret_cast<uint64_t>(&param),user));
        param.value=1;assert(BdwIntelRenderIoctl(file,setCmd,reinterpret_cast<uint64_t>(&param),user)==-22);
        assert(!BdwIntelRenderIoctl(file,getCmd,reinterpret_cast<uint64_t>(&param),user) && !param.value);
        assert(BdwIntelRenderIoctl(other,getCmd,reinterpret_cast<uint64_t>(&param),user)==-2);
        create.flags=1;assert(BdwIntelRenderIoctl(file,destroyCmd,reinterpret_cast<uint64_t>(&create),user)==-22);
        create.flags=0;assert(!BdwIntelRenderIoctl(file,destroyCmd,reinterpret_cast<uint64_t>(&create),user));
        assert(BdwIntelRenderIoctl(file,destroyCmd,reinterpret_cast<uint64_t>(&create),user)==-2);
        struct CreateContextExt {uint32_t id,flags;uint64_t extensions;} ext{};
        const auto extCmd=ioctlCommand(0x6d,sizeof(ext));ext.extensions=1;
        assert(BdwIntelRenderIoctl(file,extCmd,reinterpret_cast<uint64_t>(&ext),user)==-22);
        ext.extensions=0;assert(!BdwIntelRenderIoctl(file,extCmd,reinterpret_cast<uint64_t>(&ext),user) && ext.id);
        create.id=ext.id;assert(!BdwIntelRenderIoctl(file,destroyCmd,reinterpret_cast<uint64_t>(&create),user));
        BdwGpuFileUnref(file);BdwGpuFileUnref(other);
    }
    {
        auto* file=BdwGpuFileCreate();auto* other=BdwGpuFileCreate();assert(file && other);
        const auto first=BdwGpuFileCreateContext(file),second=BdwGpuFileCreateContext(file);
        assert(first && second && first!=second && !BdwGpuFileGetContext(other,first));
        auto* defaultA=BdwGpuFileGetDefaultContext(file);auto* defaultAgain=BdwGpuFileGetDefaultContext(file);
        auto* defaultOther=BdwGpuFileGetDefaultContext(other);
        assert(defaultA && defaultA==defaultAgain && defaultOther && defaultOther!=defaultA);
        BdwGpuContextUnref(defaultA);BdwGpuContextUnref(defaultAgain);BdwGpuContextUnref(defaultOther);
        auto* a=BdwGpuFileGetContext(file,first);auto* b=BdwGpuFileGetContext(file,second);assert(a && b);
        static uint32_t saved[20*1024],next[20*1024];bool present=true;
        assert(BdwGpuContextBegin(a,saved,present) && !present);
        assert(!BdwGpuContextBegin(a,next,present));
        next[0]=0x12345678;next[20*1024-1]=0xabcdef01;
        assert(BdwGpuContextFinish(a,&next,true));
        assert(BdwGpuContextBegin(b,saved,present) && !present && BdwGpuContextFinish(b,nullptr,true));
        assert(BdwGpuFileDestroyContext(file,first) && !BdwGpuFileGetContext(file,first));
        BdwGpuFileUnref(file); // retained context outlives its handle and file
        assert(BdwGpuContextBegin(a,saved,present) && present && saved[0]==next[0] && saved[20*1024-1]==next[20*1024-1]);
        assert(!BdwGpuContextFinish(a,nullptr,false));
        assert(BdwGpuContextLoss(a)==1);BdwGpuContextMarkLost(a,true);assert(BdwGpuContextLoss(a)==3);
        assert(!BdwGpuContextBegin(a,saved,present));
        BdwGpuContextUnref(a);BdwGpuContextUnref(b);BdwGpuFileUnref(other);
    }
    assert(!BdwGpuBufferCreate(0) && !BdwGpuBufferCreate((uint64_t{65536}<<12)+1));
    failVmm=true;assert(!BdwGpuBufferCreate(1));failVmm=false;
    badDma=true;assert(!BdwGpuBufferCreate(8192) && frees==1);badDma=false;
    failBacking=true;assert(!BdwGpuBufferCreate(4096) && frees==2);failBacking=false;
    allocated=0;auto* b=BdwGpuBufferCreate(4097);assert(b && BdwGpuBufferBytes(b)==8192);
    const uint64_t firstIdentity=BdwGpuBufferIdentity(b);assert(firstIdentity && !BdwGpuBufferIdentity(nullptr));
    assert(!BdwGpuBufferReserveHandoff(b,0));
    assert(BdwGpuBufferBeginGpuUse(b) && !BdwGpuBufferReserveHandoff(b,101) && BdwGpuBufferEndGpuUse(b));
    assert(BdwGpuBufferReserveHandoff(b,101));
    assert(!BdwGpuBufferReserveHandoff(b,102) && !BdwGpuBufferBeginGpuUse(b) && !BdwGpuBufferBeginCpuTransition(b));
    std::atomic<unsigned> admittedDuringLease{0};
    auto contender=[&](){for(unsigned i=0;i<10000;++i)if(BdwGpuBufferBeginGpuUse(b)){
        admittedDuringLease.fetch_add(1,std::memory_order_relaxed);assert(BdwGpuBufferEndGpuUse(b));}};
    std::thread contenderA(contender),contenderB(contender);contenderA.join();contenderB.join();assert(!admittedDuringLease.load());
    assert(!BdwGpuBufferBeginHandoffGpuUse(b,102) && BdwGpuBufferBeginHandoffGpuUse(b,101));
    assert(!BdwGpuBufferBeginHandoffGpuUse(b,101) && !BdwGpuBufferReleaseHandoff(b,101));
    assert(BdwGpuBufferEndGpuUse(b) && !BdwGpuBufferBeginHandoffGpuUse(b,101));
    assert(!BdwGpuBufferReleaseHandoff(b,102) && BdwGpuBufferReleaseHandoff(b,101));
    assert(BdwGpuBufferBeginGpuUse(b) && BdwGpuBufferEndGpuUse(b));
    uint32_t count=0;const auto* pages=BdwGpuBufferPages(b,count);
    assert(count==2 && pages[0]==0x100000 && pages[1]==0x101000);
    auto* bytes=reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(b,0));
    for(unsigned i=0;i<8192;++i)assert(!bytes[i]);
    BdwPpgttPage scratch[4],tables[4];
    for(unsigned i=0;i<4;++i){scratch[i]={0x10000000+uint64_t(i)*4096,pageTables[i]};tables[i]={0x20000000+uint64_t(i)*4096,pageTables[i+4]};}
    BdwExecMemoryObject object{0x10000,pages,count,true};BdwPpgttMapping mappings[2]{};
    assert(BdwBuildExecAddressSpace(scratch,tables,4,&object,1,mappings,2));
    assert(mappings[0].dma==pages[0] && mappings[1].dma==pages[1] && mappings[1].writable);
    auto* vma=BdwGpuBufferBacking(b);MappingBackingRef(vma);
    BdwGpuBufferPin pin;assert(pin.Acquire(b) && !pin.Acquire(b));
    BdwGpuBufferUnref(b);MappingBackingUnref(vma);assert(frees==2);
    assert(!pin.Retire(false) && frees==2 && pin.Get());
    assert(pin.Retire(true) && !pin.Get() && frees==3 && !pin.Retire(true));
    {
        const unsigned freesBefore=frees;allocated=0;auto* racing=BdwGpuBufferCreate(4096);assert(racing);
        assert(BdwGpuBufferIdentity(racing) && BdwGpuBufferIdentity(racing)!=firstIdentity);
        std::atomic<bool> start{false},marked{false};std::atomic<unsigned> admittedAfterFailure{0};
        auto submit=[&](){while(!start.load(std::memory_order_acquire)){}
            for(unsigned i=0;i<100000;++i){
                const bool alreadyFailed=marked.load(std::memory_order_acquire);
                if(BdwGpuBufferBeginGpuUse(racing)){
                    if(alreadyFailed)admittedAfterFailure.fetch_add(1,std::memory_order_relaxed);
                    assert(BdwGpuBufferEndGpuUse(racing));
                }
            }};
        std::thread first(submit),second(submit);start.store(true,std::memory_order_release);
        for(unsigned i=0;i<1000;++i)std::this_thread::yield();
        BdwGpuBufferMarkGpuFailed(racing);marked.store(true,std::memory_order_release);
        first.join();second.join();uint32_t uses=0;bool failed=false;
        BdwGpuBufferGpuStatus(racing,uses,failed);
        assert(failed && !uses && !admittedAfterFailure.load() && !BdwGpuBufferBeginGpuUse(racing));
        BdwGpuBufferUnref(racing);assert(frees==freesBefore+1);frees=freesBefore;
    }
    allocated=0;failAt=3;assert(!BdwGpuBufferCreate(513ull*4096) && frees==5);failAt=0;
    allocated=0;b=BdwGpuBufferCreate(513ull*4096);assert(b && allocated==3);
    pages=BdwGpuBufferPages(b,count);assert(count==513 && pages[256]==0x300000 && pages[512]==0x500000);
    for(uint32_t i=0;i<count;++i){auto* page=reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(b,i));assert(page && page[0]==0 && page[4095]==0);}
    assert(!BdwGpuBufferPageKernelAddress(b,513));BdwGpuBufferUnref(b);assert(frees==8);
    allocated=0;auto* f=BdwGpuFileCreate();auto* other=BdwGpuFileCreate();assert(f && other);
    uint64_t rounded=0;const uint32_t h=BdwGpuFileCreateBuffer(f,4097,rounded);assert(h==1 && rounded==8192);
    assert(!BdwGpuFileGetBuffer(other,h));const uint64_t token=BdwGpuFileMapToken(f,h);assert(token && token==BdwGpuFileMapToken(f,h));
    uint64_t offset=99;assert(!BdwGpuFileGetMapping(other,token,4096,offset));
    auto* mapped=BdwGpuFileGetMapping(f,token+4096,4096,offset);assert(mapped && offset==4096);
    assert(!BdwGpuFileGetMapping(f,token+4096,8192,offset));
    assert(BdwGpuFileCloseBuffer(f,h) && !BdwGpuFileCloseBuffer(f,h) && !BdwGpuFileGetMapping(f,token,4096,offset));
    // A PRIME-style import remains live independently of the original file,
    // and repeated imports into one file share a single GEM handle.
    const uint32_t imported=BdwGpuFileImportBuffer(other,mapped);assert(imported);
    assert(BdwGpuFileImportBuffer(other,mapped)==imported);
    auto* importedRef=BdwGpuFileGetBuffer(other,imported);assert(importedRef==mapped);
    BdwGpuBufferUnref(importedRef);
    assert(BdwGpuFileCloseBuffer(other,imported) && !BdwGpuFileCloseBuffer(other,imported));
    assert(BdwGpuBufferBytes(mapped)==8192);const uint32_t replacement=BdwGpuFileCreateBuffer(f,4096,rounded);
    assert(replacement==h && BdwGpuFileMapToken(f,replacement)!=token);
    const uint32_t next=BdwGpuFileCreateBuffer(f,8192,rounded);assert(next==2);
    BdwExecObjectInput exec[]={{replacement,0x20000,4096,16},{next,0x10000,4096,16|4}};
    auto* protectedOutput=BdwGpuBufferCreateHandoff(8192);assert(protectedOutput && !BdwGpuBufferAllowsUserMapping(protectedOutput));
    const uint32_t protectedHandle=BdwGpuFileImportBuffer(f,protectedOutput),protectedAlias=BdwGpuFileImportBuffer(other,protectedOutput);
    assert(protectedHandle && protectedAlias && !BdwGpuFileMapToken(f,protectedHandle) && !BdwGpuFileMapToken(other,protectedAlias));
    {BdwHandoffResources handoff{};HandoffTicket ticket{};assert(HandoffAttach(handoff.policy,72));
     assert(BdwGpuBufferAllowsUserMapping(mapped) && !BdwHandoffReserve(handoff,72,1,1,mapped,300,ticket));}
    BdwExecObjectInput protectedExec[]={{replacement,0x20000,4096,16},{protectedHandle,0x10000,4096,16|4}};
    protectedExec[1].flags=16|128;
    assert(!BdwExecSnapshotCreate(f,protectedExec,2,1,8,64));
    auto* protectedCapture=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
    assert(protectedCapture);
    BdwGpuCapture capture{};BdwExecSnapshotCapture(protectedCapture,capture);
    assert(capture.requested==1 && capture.count==0);
    assert(BdwExecSnapshotRelease(protectedCapture,false));
    protectedExec[1].flags=16|4;
    {
        BdwExecObjectInput reversed[]={{next,0x10000,4096,16|4},{replacement,0x20000,4096,16}};
        auto* output=BdwGpuFileGetBuffer(f,next);auto* busy=BdwGpuFileGetBuffer(f,replacement);
        auto* snapshot=BdwExecSnapshotCreate(f,reversed,2,1,8,64);auto* fence=BdwFenceCreate();
        assert(output && busy && snapshot && fence && BdwExecSnapshotAddSignalFence(snapshot,fence));
        assert(BdwGpuBufferReserveHandoff(output,200) && BdwExecSnapshotBindHandoff(snapshot,output,fence,200));
        assert(BdwGpuBufferBeginCpuTransition(busy) && !BdwExecSnapshotReserve(snapshot));
        assert(!BdwGpuBufferHandoffMatches(output,200) && !BdwExecSnapshotReserve(snapshot));
        assert(BdwGpuBufferEndCpuTransition(busy) && BdwGpuBufferReleaseHandoff(output,200));
        assert(BdwExecSnapshotRelease(snapshot,false));BdwFenceUnref(fence);BdwGpuBufferUnref(output);BdwGpuBufferUnref(busy);
    }
    {
        auto* output=BdwGpuFileGetBuffer(f,next);auto* snapshot=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* rival=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* fence=BdwFenceCreate();auto* wrong=BdwFenceCreate();
        assert(output && snapshot && rival && fence && wrong && BdwExecSnapshotAddSignalFence(snapshot,fence));
        assert(BdwGpuBufferReserveHandoff(output,201) && BdwGpuBufferHandoffMatches(output,201));
        assert(!BdwExecSnapshotBindHandoff(snapshot,output,wrong,201));
        assert(!BdwExecSnapshotBindHandoff(snapshot,output,fence,202));
        assert(BdwExecSnapshotBindHandoff(snapshot,output,fence,201));
        assert(!BdwExecSnapshotBindHandoff(snapshot,output,fence,201));
        assert(!BdwExecSnapshotReserve(rival) && BdwExecSnapshotReserve(snapshot));
        assert(!BdwGpuBufferReleaseHandoff(output,201) && !BdwExecSnapshotReserve(rival));
        assert(BdwExecSnapshotRetire(snapshot,false) && BdwExecSnapshotSignalFences(snapshot,true));
        assert(!BdwExecSnapshotReserve(rival) && BdwGpuBufferReleaseHandoff(output,201));
        assert(BdwExecSnapshotReserve(rival) && BdwExecSnapshotRetire(rival,false));
        assert(BdwExecSnapshotRelease(snapshot,false) && BdwExecSnapshotRelease(rival,false));
        BdwFenceUnref(fence);BdwFenceUnref(wrong);BdwGpuBufferUnref(output);
    }
    {
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* readonly=BdwGpuFileGetBuffer(f,replacement);
        auto* snapshot=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);auto* fence=BdwFenceCreate();auto* wrong=BdwFenceCreate();
        assert(output && readonly && snapshot && fence && wrong);
        assert(BdwExecSnapshotAddSignalFence(snapshot,fence));
        assert(BdwExecSnapshotWritesWithSignal(snapshot,output,fence));
        assert(!BdwExecSnapshotWritesWithSignal(snapshot,readonly,fence));
        assert(!BdwExecSnapshotWritesWithSignal(snapshot,output,wrong));
        BdwHandoffResources handoff{};HandoffTicket ticket{};auto* backing=BdwGpuBufferBacking(output);
        const uint32_t baseline=backing->references;
        assert(HandoffAttach(handoff.policy,73) && BdwHandoffReserve(handoff,73,4,9,output,302,ticket));
        assert(!BdwGpuBufferBeginGpuUse(output) && !BdwGpuBufferBeginCpuTransition(output));
        assert(backing->references==baseline+1 && !BdwHandoffArm(handoff,73,ticket,snapshot,wrong,301));
        assert(BdwHandoffArm(handoff,73,ticket,snapshot,fence,302) && BdwExecSnapshotReserve(snapshot));
        assert(!BdwHandoffObserve(handoff,ticket));BdwHandoffDetach(handoff,73);
        assert(handoff.policy.slots[ticket.slot].state==HandoffSlotState::Quarantined && backing->references==baseline+1);
        assert(BdwExecSnapshotRetire(snapshot,false) && BdwExecSnapshotSignalFences(snapshot,true));
        assert(BdwHandoffObserve(handoff,ticket)==1 && backing->references==baseline);
        assert(!BdwExecSnapshotWritesWithSignal(snapshot,output,fence));
        assert(BdwExecSnapshotRelease(snapshot,false));BdwFenceUnref(fence);BdwFenceUnref(wrong);
        BdwGpuBufferUnref(output);BdwGpuBufferUnref(readonly);
    }
    {
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* snapshot=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);auto* fence=BdwFenceCreate();
        assert(output && snapshot && fence && BdwExecSnapshotAddSignalFence(snapshot,fence));
        BdwHandoffResources handoff{};HandoffTicket ticket{};auto* backing=BdwGpuBufferBacking(output);const uint32_t baseline=backing->references;
        assert(HandoffAttach(handoff.policy,74) && BdwHandoffReserve(handoff,74,5,9,output,303,ticket));
        assert(BdwHandoffArm(handoff,74,ticket,snapshot,fence,303) && BdwExecSnapshotReserve(snapshot));
        assert(BdwExecSnapshotRetire(snapshot,false) && BdwExecSnapshotSignalFences(snapshot,true) && BdwHandoffObserve(handoff,ticket)==1);
        assert(BdwHandoffTake(handoff,ticket,5,9));BdwHandoffDetach(handoff,74);
        assert(!BdwGpuBufferBeginGpuUse(output) && !BdwGpuBufferBeginCpuTransition(output));
        assert(backing->references==baseline+1 && BdwHandoffFinish(handoff,ticket) && backing->references==baseline);
        assert(BdwGpuBufferBeginGpuUse(output) && BdwGpuBufferEndGpuUse(output));
        assert(BdwExecSnapshotRelease(snapshot,false));BdwFenceUnref(fence);BdwGpuBufferUnref(output);
    }
    {
        auto* first=BdwGpuBufferCreateHandoff(8192);auto* second=BdwGpuBufferCreateHandoff(8192);
        assert(first && second);uint32_t handles[]={BdwGpuFileImportBuffer(f,first),BdwGpuFileImportBuffer(f,second)};
        assert(handles[0] && handles[1]);BdwGpuBuffer* buffers[]={first,second};
        BdwHandoffResources handoff{};BdwScanoutResources scan{};assert(HandoffAttach(handoff.policy,900));
        auto prepare=[&](unsigned i,uint64_t token){
            BdwExecObjectInput objects[]={{replacement,0x20000,4096,16},{handles[i],0x10000,4096,16|4}};
            auto* snapshot=BdwExecSnapshotCreate(f,objects,2,0,8,64);auto* fence=BdwFenceCreate();HandoffTicket t{};
            assert(snapshot && fence && BdwExecSnapshotAddSignalFence(snapshot,fence));
            assert(BdwHandoffReserve(handoff,900,1,9,buffers[i],token,t));
            assert(!BdwScanoutAdopt(scan,handoff,t,9)); // not completed or taken
            assert(BdwHandoffArm(handoff,900,t,snapshot,fence,token) && BdwExecSnapshotReserve(snapshot));
            assert(BdwExecSnapshotRetire(snapshot,false) && BdwExecSnapshotSignalFences(snapshot,true));
            assert(BdwHandoffObserve(handoff,t)==1 && BdwHandoffTake(handoff,t,1,9));
            assert(!BdwScanoutAdopt(scan,handoff,t,10));
            assert(BdwScanoutAdopt(scan,handoff,t,9));
            assert(!BdwHandoffFinish(handoff,t) && BdwGpuBufferHandoffCompleted(buffers[i],token));
            assert(BdwExecSnapshotRelease(snapshot,false));BdwFenceUnref(fence);
        };
        struct MappingIo {
            alignas(4096) uint64_t pte[512]{};volatile uint64_t* pages[1]{pte};uint32_t flush=0;
            bool valid=true,idle=true,failInvalidate=false,takeover=false,guard=true;unsigned writes=0,failWrite=0;uint64_t prior[2]{};
            struct Owner {
                MappingIo* io;
                bool readable(){return io->valid;}bool owned(){return io->valid;}bool quiescent(){return io->idle;}
                bool validateLease(const BdwGgttExtentLease& l){return l.reservation.first==65536 && l.reservation.count==2;}
                bool validateTakeover(const BdwGgttExtentLease&,uint64_t scratch){return io->valid && io->takeover && scratch==0x400000;}
                bool auditPtePage(uint32_t index,volatile uint64_t* p){return index==128 && p==io->pte;}
                bool invalidateGpuTranslations(){return !io->failInvalidate;}
            } owner{this};
            BdwGgttExtentMmio<Owner>* accessor=nullptr;
            ~MappingIo(){delete accessor;}
            bool validateLease(const BdwGgttExtentLease& l){
                if(!accessor)accessor=new BdwGgttExtentMmio<Owner>(owner,l,pages,1,&flush,takeover?0x400000:0);
                return accessor && accessor->validateLease(l);
            }
            bool validateTakeover(const BdwGgttExtentLease& l,uint64_t scratch){return accessor && accessor->validateTakeover(l,scratch);}
            bool validateDisplayGuard(const BdwGgttReservation& full,const BdwGgttExtentLease& l){
                return guard && full.first==l.reservation.first && full.count==l.reservation.count+128 &&
                    full.token==l.reservation.token && !(full.first&63);
            }
            bool prepareTakeover(const BdwGgttExtentLease& l,const uint64_t*& before,uint64_t& scratch){
                if(!takeover || !validateLease(l))return false;
                prior[0]=pte[0];prior[1]=pte[1];before=prior;scratch=0x400000;return true;
            }
            bool quiescent(){return idle;}
            bool read(uint32_t i,uint64_t& v){return accessor && accessor->read(i,v);}
            bool write(uint32_t i,uint64_t v){if(!accessor || !accessor->write(i,v))return false;return ++writes!=failWrite;}
            bool invalidate(){return accessor && !failInvalidate && accessor->invalidate();}
        } io;
        static BdwGgttSpace scanSpace;assert(scanSpace.configure(262144));
        BdwScanoutGgttMapping<MappingIo> mapping(scanSpace,io);
        prepare(0,9001);
        assert(!BdwGpuBufferBeginScanoutMapping(first,9999));
        pid_t noGuard=fork();assert(noGuard>=0);
        if(!noGuard){io.guard=false;assert(mapping.Bind(scan,1)==BdwScanoutResult::Rejected && !mapping.Held());
            assert(io.writes==0 && scan.policy.pending==BdwGpuBufferIdentity(first));_exit(0);}
        int noGuardStatus=0;assert(waitpid(noGuard,&noGuardStatus,0)==noGuard && WIFEXITED(noGuardStatus) && WEXITSTATUS(noGuardStatus)==0);
        pid_t takeoverChild=fork();assert(takeoverChild>=0);
        if(!takeoverChild){
            io.takeover=true;io.pte[0]=0xdead0001;io.pte[1]=0xbeef0000;
            assert(mapping.BindTakenOver(scan,1)==BdwScanoutResult::Completed && mapping.Held());
            assert(BdwScanoutCancelUnissued(scan)==BdwScanoutResult::Retained);
            assert(mapping.Retire(scan)==BdwScanoutResult::Completed && !mapping.Held());
            assert(!io.pte[0] && !io.pte[1] && BdwScanoutCollectRetired(scan));_exit(0);
        }
        int takeoverStatus=0;assert(waitpid(takeoverChild,&takeoverStatus,0)==takeoverChild && WIFEXITED(takeoverStatus) && WEXITSTATUS(takeoverStatus)==0);
        pid_t takeoverFailure=fork();assert(takeoverFailure>=0);
        if(!takeoverFailure){
            io.takeover=true;io.pte[0]=0xdead0001;io.pte[1]=0xbeef0000;io.failWrite=2;io.failInvalidate=true;
            assert(mapping.BindTakenOver(scan,1)==BdwScanoutResult::Retained && mapping.Held());
            assert(scanSpace.owns(mapping.Reservation()) && !BdwGpuBufferReleaseHandoff(first,9001));
            assert(BdwScanoutCancelUnissued(scan)==BdwScanoutResult::Retained);
            assert(mapping.Retire(scan)==BdwScanoutResult::Retained && mapping.Held());_exit(0);
        }
        int takeoverFailureStatus=0;assert(waitpid(takeoverFailure,&takeoverFailureStatus,0)==takeoverFailure && WIFEXITED(takeoverFailureStatus) && WEXITSTATUS(takeoverFailureStatus)==0);
        pid_t mappingChild=fork();assert(mappingChild>=0);
        if(!mappingChild){
            io.failWrite=2;io.failInvalidate=true;
            assert(mapping.Bind(scan,1)==BdwScanoutResult::Retained && mapping.Held());
            assert(scanSpace.owns(mapping.Reservation()) && !BdwGpuBufferReleaseHandoff(first,9001));
            assert(BdwScanoutCancelUnissued(scan)==BdwScanoutResult::Retained);
            assert(mapping.Retire(scan)==BdwScanoutResult::Retained && mapping.Held());_exit(0);
        }
        int mappingStatus=0;assert(waitpid(mappingChild,&mappingStatus,0)==mappingChild && WIFEXITED(mappingStatus) && WEXITSTATUS(mappingStatus)==0);
        assert(mapping.Bind(scan,1)==BdwScanoutResult::Completed && mapping.Held());
        assert(mapping.Reservation().count==130 && !(mapping.Reservation().first&63));
        BdwGgttReservation neighbor{};assert(scanSpace.reserve(1,neighbor));
        assert(neighbor.first>=mapping.Reservation().first+mapping.Reservation().count && scanSpace.release(neighbor));
        uint32_t pageCount=0;const auto* pages=BdwGpuBufferPages(first,pageCount);
        assert(pageCount==2 && io.pte[0]==(pages[0]|1) && io.pte[1]==(pages[1]|1));
        assert(mapping.Retire(scan)==BdwScanoutResult::Rejected && ScanoutIssue(scan.policy));
        assert(BdwScanoutComplete(scan,9,BdwGpuBufferIdentity(first))==BdwScanoutResult::Completed);
        BdwHandoffDetach(handoff,900);assert(BdwGpuFileCloseBuffer(f,handles[0]));
        assert(BdwGpuBufferBacking(first)->references>=2 && !BdwGpuBufferBeginGpuUse(first));
        assert(HandoffAttach(handoff.policy,900));prepare(1,9002);assert(ScanoutIssue(scan.policy));
        assert(BdwScanoutComplete(scan,9,123)==BdwScanoutResult::Rejected);
        assert(!BdwGpuBufferBeginGpuUse(first) && !BdwGpuBufferBeginGpuUse(second));
        // Failure during retirement must leave the old reference quarantined;
        // isolate the deliberately unrecoverable sticky GPU failure.
        pid_t child=fork();assert(child>=0);
        if(!child){
            BdwGpuBufferMarkGpuFailed(first);
            assert(BdwScanoutComplete(scan,9,BdwGpuBufferIdentity(second))==BdwScanoutResult::Retained);
            assert(scan.policy.active==BdwGpuBufferIdentity(second) && !BdwScanoutCollectRetired(scan));
            assert(BdwGpuBufferBacking(first)->references>=2 && !BdwGpuBufferBeginGpuUse(first));
            assert(BdwScanoutQuiesced(scan)==BdwScanoutResult::Retained);
            assert(BdwGpuBufferBacking(first)->references>=2);_exit(0);
        }
        int childStatus=0;assert(waitpid(child,&childStatus,0)==child && WIFEXITED(childStatus) && WEXITSTATUS(childStatus)==0);
        assert(!BdwGpuBufferBeginScanoutMapping(first,9001) && !BdwGpuBufferReleaseHandoff(first,9001));
        assert(BdwScanoutComplete(scan,9,BdwGpuBufferIdentity(second))==BdwScanoutResult::Retained);
        assert(!BdwGpuBufferBeginGpuUse(first) && !BdwGpuBufferEndScanoutMapping(first,9999));
        assert(mapping.Retire(scan)==BdwScanoutResult::Completed && !mapping.Held() && !io.pte[0] && !io.pte[1]);
        assert(!BdwGpuBufferEndScanoutMapping(first,9001));
        assert(BdwScanoutCollectRetired(scan));
        assert(BdwGpuBufferBeginGpuUse(first) && BdwGpuBufferEndGpuUse(first));
        assert(!BdwGpuBufferBeginGpuUse(second));ScanoutUncertain(scan.policy);
        assert(BdwScanoutQuiesced(scan)==BdwScanoutResult::Completed);
        assert(BdwGpuBufferBeginGpuUse(second) && BdwGpuBufferEndGpuUse(second));
        BdwHandoffDetach(handoff,900);assert(BdwGpuFileCloseBuffer(f,handles[1]));
        BdwGpuBufferUnref(first);BdwGpuBufferUnref(second);
    }
    // A blocked dependency may not stall a separate owner with a distinct
    // context and disjoint buffers. Unknown identity, a shared context, a
    // shared buffer, or the same owner preserve admission order.
    for(unsigned mode=0;mode<5;++mode){
        auto* ca=BdwGpuFileGetDefaultContext(f);auto* cb=BdwGpuFileGetDefaultContext(other);assert(ca && cb && ca!=cb);
        BdwExecObjectInput firstObject{replacement,0x10000,4096,16};
        BdwExecObjectInput secondObject{mode==2?replacement:next,0x20000,4096,16};
        auto* first=BdwExecSnapshotCreate(f,&firstObject,1,0,0,8);auto* second=BdwExecSnapshotCreate(f,&secondObject,1,0,0,8);
        auto* wait=BdwFenceCreate();assert(first && second && wait);
        assert(BdwExecSnapshotBindContext(first,ca) && BdwExecSnapshotBindContext(second,mode==1?ca:cb));
        BdwExecSnapshotSetOrigin(first,123,0,76);BdwExecSnapshotSetOrigin(second,124,0,mode==3?76:mode==4?0:77);
        assert(BdwExecSnapshotAddWaitFence(first,wait));BdwExecQueue queue;uint64_t a=0,b=0,id=0;BdwExecSnapshot* item=nullptr;
        assert(queue.Enqueue(1,first,a) && queue.Enqueue(1,second,b));
        if(mode==0){assert(queue.StartNext(id,item) && id==b && item==second && queue.Finish(id,true,true));
            assert(!queue.StartNext(id,item));assert(BdwFenceComplete(wait,true));assert(queue.StartNext(id,item) && id==a);}
        else {assert(!queue.StartNext(id,item));assert(BdwFenceComplete(wait,true));assert(queue.StartNext(id,item) && id==a);}
        assert(queue.Finish(a,true,true));BdwExecRequestState state{};
        assert(queue.Reap(1,a,state)==first && BdwExecSnapshotRelease(first,false));
        if(mode!=0)assert(queue.StartNext(id,item) && id==b && queue.Finish(id,true,true));
        assert(queue.Reap(1,b,state)==second && BdwExecSnapshotRelease(second,false));
        BdwFenceUnref(wait);BdwGpuContextUnref(ca);BdwGpuContextUnref(cb);
    }
    {
        auto* thirdFile=BdwGpuFileCreate();assert(thirdFile);
        auto* aCtx=BdwGpuFileGetDefaultContext(f);auto* bCtx=BdwGpuFileGetDefaultContext(other);
        auto* cCtx=BdwGpuFileGetDefaultContext(thirdFile);assert(aCtx && bCtx && cCtx);
        BdwExecObjectInput aObj{replacement,0x10000,4096,16},bObj{next,0x20000,4096,16};
        auto* a=BdwExecSnapshotCreate(f,&aObj,1,0,0,8);auto* b=BdwExecSnapshotCreate(f,&bObj,1,0,0,8);
        auto* c=BdwExecSnapshotCreate(f,&bObj,1,0,0,8);auto* wait=BdwFenceCreate();assert(a && b && c && wait);
        assert(BdwExecSnapshotBindContext(a,aCtx) && BdwExecSnapshotBindContext(b,bCtx) && BdwExecSnapshotBindContext(c,cCtx));
        BdwExecSnapshotSetOrigin(a,1,0,76);BdwExecSnapshotSetOrigin(b,2,0,76);BdwExecSnapshotSetOrigin(c,3,0,77);
        assert(BdwExecSnapshotAddWaitFence(a,wait));BdwExecQueue queue;uint64_t aid=0,bid=0,cid=0,id=0;BdwExecSnapshot* item=nullptr;
        assert(queue.Enqueue(1,a,aid) && queue.Enqueue(1,b,bid) && queue.Enqueue(1,c,cid));
        assert(!queue.StartNext(id,item)); // c cannot pass b's shared buffer
        assert(BdwFenceComplete(wait,true));BdwExecRequestState state{};
        const uint64_t expectedIds[]={aid,bid,cid};
        for(auto expected:expectedIds){assert(queue.StartNext(id,item) && id==expected && queue.Finish(id,true,true));
            auto* retired=queue.Reap(1,id,state);assert(retired && BdwExecSnapshotRelease(retired,false));}
        BdwFenceUnref(wait);BdwGpuContextUnref(aCtx);BdwGpuContextUnref(bCtx);BdwGpuContextUnref(cCtx);BdwGpuFileUnref(thirdFile);
    }
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* snapshot=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);auto* fence=BdwFenceCreate();
        assert(output && snapshot && fence && BdwExecSnapshotAddSignalFence(snapshot,fence));
        BdwHandoffResources h{};HandoffTicket ticket{};assert(HandoffAttach(h.policy,75));
        assert(BdwHandoffReserve(h,75,6,9,output,304,ticket) && BdwHandoffArm(h,75,ticket,snapshot,fence,304));
        assert(BdwExecSnapshotReserve(snapshot) && BdwExecSnapshotRetire(snapshot,false));
        assert(BdwExecSnapshotSignalFences(snapshot,true) && BdwHandoffObserve(h,ticket)==1 && BdwHandoffTake(h,ticket,6,9));
        BdwGpuBufferMarkGpuFailed(output);assert(!BdwHandoffFinish(h,ticket));
        assert(h.policy.slots[ticket.slot].state==HandoffSlotState::Quarantined && h.buffers[ticket.slot]==output && h.tokens[ticket.slot]==304);
        _exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    // Race a dependency becoming ready and worker admission against an
    // owner-bound cancellation. Exactly one side may acquire the snapshot.
    for(unsigned iteration=0;iteration<32;++iteration){pid_t child=fork();assert(child>=0);if(!child){
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* snapshot=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* wait=BdwFenceCreate();auto* signal=BdwFenceCreate();assert(output && snapshot && wait && signal);
        assert(BdwExecSnapshotAddWaitFence(snapshot,wait) && BdwExecSnapshotAddSignalFence(snapshot,signal));
        BdwHandoffResources h{};HandoffTicket ticket{};BdwSubmissionQueue service;assert(service.Init());
        assert(HandoffAttach(h.policy,76) && BdwHandoffReserve(h,76,7,9,output,310,ticket));
        assert(!service.SubmitHandoff(h,76,ticket,snapshot,signal,310));
        std::atomic<bool> go{false};bool admitted=false;uint64_t active=0;BdwExecSnapshot* item=nullptr;int64_t cancelled=-99;
        std::thread admission([&]{while(!go.load(std::memory_order_acquire))std::this_thread::yield();
            assert(BdwFenceComplete(wait,true));admitted=service.Take(active,item);});
        std::thread cancellation([&]{while(!go.load(std::memory_order_acquire))std::this_thread::yield();
            cancelled=service.CancelBlockedHandoff(h,76,ticket);});
        go.store(true,std::memory_order_release);admission.join();cancellation.join();
        if(!cancelled){assert(!admitted && BdwFenceState(signal)==2 && h.policy.slots[ticket.slot].state==HandoffSlotState::Free);}
        else {assert(cancelled==-11 && admitted && item==snapshot && service.Finish(active,true,true));
            assert(BdwHandoffObserve(h,ticket)==1 && BdwHandoffTake(h,ticket,7,9) && BdwHandoffFinish(h,ticket));}
        _exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    {
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* snapshot=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);auto* fence=BdwFenceCreate();
        assert(output && snapshot && fence && BdwExecSnapshotAddSignalFence(snapshot,fence));
        BdwHandoffResources handoff{};HandoffTicket ticket{};FakeWorker host;BdwSubmissionWorker worker;
        assert(HandoffAttach(handoff.policy,76) && BdwHandoffReserve(handoff,76,7,9,output,305,ticket) && worker.Start(host.Hooks(),0));
        assert(!worker.SubmitHandoff(handoff,76,ticket,snapshot,fence,305));
        assert(worker.CancelBlockedHandoff(handoff,76,ticket)==-11); // ready is not dependency-blocked
        assert(!BdwGpuBufferBeginGpuUse(output));BdwHandoffDetach(handoff,76);
        assert(handoff.policy.slots[ticket.slot].state==HandoffSlotState::Quarantined);
        assert(worker.Step()==BdwSubmissionStep::Finished && BdwHandoffObserve(handoff,ticket)==1);
        assert(handoff.policy.slots[ticket.slot].state==HandoffSlotState::Free && BdwGpuBufferBeginGpuUse(output));
        assert(BdwGpuBufferEndGpuUse(output));BdwFenceUnref(fence);BdwGpuBufferUnref(output);
    }
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* snapshot=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* wait=BdwFenceCreate();auto* signal=BdwFenceCreate();assert(output && snapshot && wait && signal);
        assert(BdwExecSnapshotAddWaitFence(snapshot,wait) && BdwExecSnapshotAddSignalFence(snapshot,signal));
        BdwHandoffResources h{};HandoffTicket ticket{};FakeWorker host;BdwSubmissionWorker worker;
        assert(HandoffAttach(h.policy,76) && BdwHandoffReserve(h,76,7,9,output,308,ticket) && worker.Start(host.Hooks(),0));
        assert(!worker.SubmitHandoff(h,76,ticket,snapshot,signal,308));
        assert(worker.Step()==BdwSubmissionStep::Idle && !host.executions);
        assert(worker.CancelBlockedHandoff(h,77,ticket)==-2);
        assert(!worker.CancelBlockedHandoff(h,76,ticket) && BdwFenceState(signal)==2);
        assert(h.policy.slots[ticket.slot].state==HandoffSlotState::Free);
        assert(BdwGpuBufferBeginGpuUse(output) && BdwGpuBufferEndGpuUse(output));
        HandoffTicket replacementTicket{};assert(BdwHandoffReserve(h,76,7,9,output,310,replacementTicket));
        assert(replacementTicket.ticket!=ticket.ticket && BdwHandoffCancel(h,76,replacementTicket));
        for(uint64_t n=0;n<4;++n){
            auto* again=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);auto* dependency=BdwFenceCreate();auto* completion=BdwFenceCreate();
            HandoffTicket nextTicket{};assert(again && dependency && completion);
            assert(BdwExecSnapshotAddWaitFence(again,dependency) && BdwExecSnapshotAddSignalFence(again,completion));
            assert(BdwHandoffReserve(h,76,7,9,output,320+n,nextTicket));
            assert(!worker.SubmitHandoff(h,76,nextTicket,again,completion,320+n));
            assert(!worker.CancelBlockedHandoff(h,76,nextTicket) && BdwFenceState(completion)==2);
            assert(h.policy.slots[nextTicket.slot].state==HandoffSlotState::Free && BdwGpuBufferBeginGpuUse(output));
            assert(BdwGpuBufferEndGpuUse(output));BdwFenceUnref(dependency);BdwFenceUnref(completion);
        }
        auto* next=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(next && !worker.Submit(next));
        assert(worker.Step()==BdwSubmissionStep::Finished && host.executions==1);
        _exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    {
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* snapshot=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* fence=BdwFenceCreate();assert(output && snapshot && fence && BdwExecSnapshotAddSignalFence(snapshot,fence));
        BdwHandoffResources h{};HandoffTicket ticket{};BdwSubmissionQueue service;assert(service.Init());
        assert(HandoffAttach(h.policy,76) && BdwHandoffReserve(h,76,7,9,output,309,ticket));
        assert(!service.SubmitHandoff(h,76,ticket,snapshot,fence,309));
        uint64_t active=0;BdwExecSnapshot* item=nullptr;assert(service.Take(active,item) && item==snapshot);
        assert(service.CancelBlockedHandoff(h,76,ticket)==-11 && !BdwFenceState(fence));
        assert(service.Finish(active,true,true) && BdwHandoffObserve(h,ticket)==1);
        assert(BdwHandoffTake(h,ticket,7,9) && BdwHandoffFinish(h,ticket));
        BdwHandoffDetach(h,76);BdwFenceUnref(fence);BdwGpuBufferUnref(output);
    }
    {
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* busy=BdwGpuFileGetBuffer(f,replacement);
        BdwExecObjectInput reversed[]={{protectedHandle,0x10000,4096,16|4},{replacement,0x20000,4096,16}};
        auto* snapshot=BdwExecSnapshotCreate(f,reversed,2,1,8,64);auto* fence=BdwFenceCreate();
        assert(output && busy && snapshot && fence && BdwExecSnapshotAddSignalFence(snapshot,fence));
        BdwHandoffResources handoff{};HandoffTicket ticket{};FakeWorker host;BdwSubmissionWorker worker;
        assert(HandoffAttach(handoff.policy,77) && BdwHandoffReserve(handoff,77,8,9,output,306,ticket) && worker.Start(host.Hooks(),0));
        assert(BdwGpuBufferBeginCpuTransition(busy));
        assert(worker.SubmitHandoff(handoff,77,ticket,snapshot,fence,306)==-16);
        assert(handoff.policy.slots[ticket.slot].state==HandoffSlotState::Free && !BdwFenceState(fence));
        assert(BdwGpuBufferEndCpuTransition(busy) && BdwGpuBufferBeginGpuUse(output) && BdwGpuBufferEndGpuUse(output));
        assert(BdwExecSnapshotRelease(snapshot,false));BdwFenceUnref(fence);BdwGpuBufferUnref(output);BdwGpuBufferUnref(busy);
    }
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* snapshot=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);auto* fence=BdwFenceCreate();
        assert(output && snapshot && fence && BdwExecSnapshotAddSignalFence(snapshot,fence));
        BdwHandoffResources h{};HandoffTicket ticket{};FakeWorker host;host.success=false;host.safelyStopped=false;BdwSubmissionWorker worker;
        assert(HandoffAttach(h.policy,78) && BdwHandoffReserve(h,78,9,9,output,307,ticket) && worker.Start(host.Hooks(),0));
        assert(!worker.SubmitHandoff(h,78,ticket,snapshot,fence,307));
        assert(worker.Step()==BdwSubmissionStep::Recover);BdwHandoffDetach(h,78);
        assert(h.policy.slots[ticket.slot].state==HandoffSlotState::Quarantined && h.buffers[ticket.slot]==output);
        assert(!BdwHandoffObserve(h,ticket) && BdwGpuBufferGpuFailed(output) && !BdwGpuBufferBeginGpuUse(output));
        _exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    {
        struct RawObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
        struct RawFence {uint32_t handle,flags;};
        struct RawExec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);assert(output);
        const auto syncId=BdwGpuFileCreateSync(f,false);assert(syncId);
        RawObject objects[]={{replacement,0,0,4096,0x20000,16,0,0},{protectedHandle,0,0,4096,0x10000,20,0,0}};
        RawFence signal{syncId,2};RawExec request{reinterpret_cast<uint64_t>(objects),2,0,8,0,0,1,
            reinterpret_cast<uint64_t>(&signal),(1u<<11)|(1u<<18)|(1u<<19),0,0};
        BdwHandoffResources handoff{};HandoffTicket ticket{};FakeWorker host;BdwSubmissionWorker worker;
        assert(HandoffAttach(handoff.policy,79) && BdwHandoffReserve(handoff,79,10,9,output,308,ticket) && worker.Start(host.Hooks(),0));
        FakeUser userState{};userState.handoffWorker=&worker;userState.handoff=&handoff;userState.handoffBuffer=output;
        userState.handoffTicket=ticket;userState.handoffOwner=79;userState.handoffToken=308;
        BdwI915UserAccess user{&userState,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        user.submit=UserSubmit;user.renderExperiment=true;user.handoffSubmit=UserHandoffSubmit;
        request.clipCount=0;request.clips=0;request.flags&=~(1u<<19);
        assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(request)),reinterpret_cast<uint64_t>(&request),user)==-22);
        assert(userState.handoffCalls==1 && !userState.request && handoff.policy.slots[ticket.slot].state==HandoffSlotState::Reserved);
        request.clipCount=1;request.clips=reinterpret_cast<uint64_t>(&signal);request.flags|=1u<<19;
        assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(request)),reinterpret_cast<uint64_t>(&request),user));
        assert(userState.handoffCalls==2 && !userState.request && !BdwGpuBufferBeginGpuUse(output));
        auto* sync=BdwGpuFileGetSync(f,syncId);assert(sync);auto* submitted=BdwGpuSyncAcquire(sync);assert(submitted && !BdwFenceState(submitted));
        BdwHandoffDetach(handoff,79);assert(worker.Step()==BdwSubmissionStep::Finished);
        assert(BdwFenceState(submitted)==1 && BdwHandoffObserve(handoff,ticket)==1);
        assert(handoff.policy.slots[ticket.slot].state==HandoffSlotState::Free && BdwGpuBufferBeginGpuUse(output));
        assert(BdwGpuBufferEndGpuUse(output));BdwFenceUnref(submitted);BdwGpuSyncUnref(sync);
        assert(BdwGpuFileDestroySync(f,syncId));BdwGpuBufferUnref(output);
    }
    {
        struct RawObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
        struct RawFence {uint32_t handle,flags;};
        struct RawExec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);assert(output);
        BdwHandoffSession session;FakeWorker host;BdwSubmissionWorker worker;assert(worker.Start(host.Hooks(),0));
        assert(session.Attach(80) && !session.Attach(81));
        assert(BdwGpuBufferBeginGpuUse(output));assert(session.Reserve(80,output)==-16);assert(BdwGpuBufferEndGpuUse(output));
        auto ticket=session.Reserve(80,output);assert(ticket>0 && !BdwGpuBufferBeginGpuUse(output));
        assert(session.Observe(81,ticket)==-2 && session.Release(81,ticket)==-2);
        const auto syncId=BdwGpuFileCreateSync(f,false);assert(syncId);
        RawObject objects[]={{replacement,0,0,4096,0x20000,16,0,0},{protectedHandle,0,0,4096,0x10000,20,0,0}};
        RawFence signal{syncId,2};RawExec request{reinterpret_cast<uint64_t>(objects),2,0,8,0,0,1,
            reinterpret_cast<uint64_t>(&signal),(1u<<11)|(1u<<18)|(1u<<19),0,0};
        FakeUser userState{};userState.handoffWorker=&worker;userState.session=&session;userState.handoffOwner=81;
        BdwI915UserAccess user{&userState,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        user.submit=UserSubmit;user.renderExperiment=true;user.handoffSubmit=UserSessionSubmit;
        assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(request)),reinterpret_cast<uint64_t>(&request),user)==-16);
        userState.handoffOwner=80;request.clipCount=0;request.clips=0;request.flags&=~(1u<<19);
        assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(request)),reinterpret_cast<uint64_t>(&request),user)==-22);
        request.clipCount=1;request.clips=reinterpret_cast<uint64_t>(&signal);request.flags|=1u<<19;
        assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(request)),reinterpret_cast<uint64_t>(&request),user));
        assert(session.Observe(80,ticket)==0 && session.Release(80,ticket)==-16);
        session.Detach(80);assert(session.Attach(81));
        assert(!BdwGpuBufferBeginGpuUse(output));assert(worker.Step()==BdwSubmissionStep::Finished);
        session.Detach(81);assert(session.Attach(82)); // reaps retired orphan
        assert(BdwGpuBufferBeginGpuUse(output) && BdwGpuBufferEndGpuUse(output));
        assert(session.OwnedBy(82) && session.Idle());
        auto nextTicket=session.Reserve(82,output,17,23);assert(nextTicket>ticket && !session.Idle());
        userState.handoffOwner=82;
        assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(request)),reinterpret_cast<uint64_t>(&request),user));
        bool called=false;auto present=[&](BdwGpuBuffer* b,uint64_t token,uint64_t scene,uint64_t display)->int64_t{
            called=true;assert(scene==17 && display==23 && b==output && !BdwGpuBufferBeginGpuUse(b));
            assert(!BdwGpuBufferPrepareHandoffRead(b,token+1,0,4));
            assert(!BdwGpuBufferPrepareHandoffRead(b,token,BdwGpuBufferBytes(b)-1,2));
            assert(BdwGpuBufferPrepareHandoffRead(b,token,5,97));
            session.Detach(82);return 7;
        };
        assert(session.PresentProbe(82,nextTicket,present)==-16 && !called);
        assert(worker.Step()==BdwSubmissionStep::Finished && session.Observe(82,nextTicket)==1);
        assert(session.PresentProbe(81,nextTicket,present)==-2 && !called);
        assert(session.PresentProbe(82,nextTicket,present)==7 && called);
        assert(session.Attach(82));
        assert(BdwGpuBufferBeginGpuUse(output) && BdwGpuBufferEndGpuUse(output));
        auto cancelled=session.Reserve(82,output);assert(cancelled>nextTicket && session.Release(82,cancelled)==0);
        session.Detach(82);assert(BdwGpuFileDestroySync(f,syncId));BdwGpuBufferUnref(output);
    }
    {
        // Two independently completed outputs may arrive out of order. A
        // later successful presentation must prevent an older frame replacing
        // it, while a rejected later presentation must not advance the order.
        struct RawObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
        struct RawFence {uint32_t handle,flags;};
        struct RawExec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
        auto* a=BdwGpuFileGetBuffer(f,protectedHandle);auto* b=BdwGpuBufferCreateHandoff(8192);assert(a && b);
        const auto handleB=BdwGpuFileImportBuffer(f,b);assert(handleB);
        const auto syncA=BdwGpuFileCreateSync(f,false),syncB=BdwGpuFileCreateSync(f,false);assert(syncA && syncB);
        RawObject objectsA[]={{replacement,0,0,4096,0x20000,16,0,0},{protectedHandle,0,0,4096,0x10000,20,0,0}};
        RawObject objectsB[]={{replacement,0,0,4096,0x20000,16,0,0},{handleB,0,0,4096,0x10000,20,0,0}};
        RawFence signals[]={{syncA,2},{syncB,2}};
        RawExec requests[]={{reinterpret_cast<uint64_t>(objectsA),2,0,8,0,0,1,reinterpret_cast<uint64_t>(&signals[0]),(1u<<11)|(1u<<18)|(1u<<19),0,0},
                            {reinterpret_cast<uint64_t>(objectsB),2,0,8,0,0,1,reinterpret_cast<uint64_t>(&signals[1]),(1u<<11)|(1u<<18)|(1u<<19),0,0}};
        BdwHandoffSession session;FakeWorker host;BdwSubmissionWorker worker;assert(worker.Start(host.Hooks(),0) && session.Attach(90));
        FakeUser userState{};userState.handoffWorker=&worker;userState.session=&session;userState.handoffOwner=90;
        BdwI915UserAccess user{&userState,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        user.submit=UserSubmit;user.renderExperiment=true;user.handoffSubmit=UserSessionSubmit;
        for(unsigned round=0;round<2;++round){
            auto older=session.Reserve(90,a,11+round,1),newer=session.Reserve(90,b,12+round,1);assert(older>0 && newer>older);
            for(auto& request:requests)assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(request)),reinterpret_cast<uint64_t>(&request),user));
            assert(worker.Step()==BdwSubmissionStep::Finished && worker.Step()==BdwSubmissionStep::Finished);
            assert(session.Observe(90,newer)==1);
            unsigned presentations=0;
            auto accept=[&](BdwGpuBuffer* output,uint64_t token,uint64_t,uint64_t)->int64_t{
                assert(BdwGpuBufferPrepareHandoffRead(output,token,4093,32));
                ++presentations;return 0;
            };
            auto reject=[](BdwGpuBuffer*,uint64_t,uint64_t,uint64_t)->int64_t{return -5;};
            if(!round){assert(session.PresentProbe(90,newer,accept)==0 && presentations==1);
                assert(session.PresentProbe(90,older,accept)==-16 && presentations==1);
                assert(session.Observe(90,older)==1);
                assert(session.PresentProbe(90,older,accept)==-116 && presentations==1);}
            else {assert(session.PresentProbe(90,newer,reject)==-5);
                assert(session.Observe(90,older)==1);
                assert(session.PresentProbe(90,older,accept)==0 && presentations==1);}
            assert(session.Idle());
        }
        assert(session.Detach(90) && BdwGpuFileDestroySync(f,syncA) && BdwGpuFileDestroySync(f,syncB));
        assert(BdwGpuFileCloseBuffer(f,handleB));BdwGpuBufferUnref(a);BdwGpuBufferUnref(b);
    }
    {
        BdwSubmissionQueue service;assert(service.Init());BdwExecOwnerWork work{};
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* s=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* fence=BdwFenceCreate();assert(output && s && fence && BdwExecSnapshotAddSignalFence(s,fence));
        BdwExecSnapshotSetOrigin(s,123,0,100);BdwHandoffResources h{};HandoffTicket ticket{};
        assert(HandoffAttach(h.policy,100) && BdwHandoffReserve(h,100,7,9,output,311,ticket));
        assert(service.FreezeOwner(100,work) && service.SubmitHandoff(h,100,ticket,s,fence,311)==-16);
        assert(h.policy.slots[ticket.slot].state==HandoffSlotState::Reserved && !BdwFenceState(fence));
        assert(service.ThawOwner(100) && !service.SubmitHandoff(h,100,ticket,s,fence,311));
        uint64_t id=0;BdwExecSnapshot* item=nullptr;assert(service.Take(id,item) && item==s && service.Finish(id,true,true));
        assert(BdwHandoffObserve(h,ticket)==1 && BdwHandoffTake(h,ticket,7,9) && BdwHandoffFinish(h,ticket));
        BdwHandoffDetach(h,100);BdwFenceUnref(fence);BdwGpuBufferUnref(output);
    }
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* s=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* wait=BdwFenceCreate();auto* signal=BdwFenceCreate();assert(output && s && wait && signal);
        BdwExecSnapshotSetOrigin(s,123,0,76);
        assert(BdwExecSnapshotAddWaitFence(s,wait) && BdwExecSnapshotAddSignalFence(s,signal));
        BdwHandoffResources h{};HandoffTicket ticket{};BdwSubmissionQueue service;assert(service.Init());
        assert(HandoffAttach(h.policy,76) && BdwHandoffReserve(h,76,7,9,output,312,ticket));
        assert(!service.SubmitHandoff(h,76,ticket,s,signal,312));
        auto* priorUnknown=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(priorUnknown && !service.Submit(priorUnknown));
        assert(service.FreezeAndCancelBlockedHandoff(h,77,ticket)==-2);
        assert(!service.FreezeAndCancelBlockedHandoff(h,76,ticket) && BdwFenceState(signal)==2);
        BdwExecOwnerWork work{};assert(!service.FrozenOwnerWork(77,work));
        assert(service.FrozenOwnerWork(76,work) && !work.queued && !work.active && !work.quarantined && work.unknown==1);
        uint64_t id=0;BdwExecSnapshot* item=nullptr;
        assert(service.Take(id,item) && item==priorUnknown && service.Finish(id,true,true));
        assert(service.FrozenOwnerWork(76,work) && !work.unknown);
        auto* same=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* unknown=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* other=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(same && unknown && other);
        BdwExecSnapshotSetOrigin(same,123,0,76);BdwExecSnapshotSetOrigin(other,123,0,77);
        assert(service.Submit(same)==-16 && service.Submit(unknown)==-16 && !service.Submit(other));
        assert(BdwExecSnapshotRelease(same,false) && BdwExecSnapshotRelease(unknown,false));
        assert(service.Take(id,item) && item==other && service.Finish(id,true,true));
        assert(h.policy.slots[ticket.slot].state==HandoffSlotState::Free);
        assert(!service.ThawOwner(77) && service.ThawOwner(76));_exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* s=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* wait=BdwFenceCreate();auto* signal=BdwFenceCreate();assert(output && s && wait && signal);
        BdwExecSnapshotSetOrigin(s,123,0,77);
        assert(BdwExecSnapshotAddWaitFence(s,wait) && BdwExecSnapshotAddSignalFence(s,signal));
        BdwHandoffResources h{};HandoffTicket ticket{};BdwSubmissionQueue service;assert(service.Init());
        assert(HandoffAttach(h.policy,76) && BdwHandoffReserve(h,76,7,9,output,313,ticket));
        assert(!service.SubmitHandoff(h,76,ticket,s,signal,313));
        assert(service.FreezeAndCancelBlockedHandoff(h,76,ticket)==-11);
        BdwExecOwnerWork work{};assert(!service.FrozenOwnerWork(76,work) && !BdwFenceState(signal));
        _exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* s=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* wait=BdwFenceCreate();auto* signal=BdwFenceCreate();assert(output && s && wait && signal);
        assert(BdwExecSnapshotAddWaitFence(s,wait) && BdwExecSnapshotAddSignalFence(s,signal));
        BdwHandoffResources h{};HandoffTicket ticket{};BdwSubmissionQueue service;assert(service.Init());
        assert(HandoffAttach(h.policy,76) && BdwHandoffReserve(h,76,7,9,output,315,ticket));
        assert(!service.SubmitHandoff(h,76,ticket,s,signal,315));
        ++h.tokens[ticket.slot]; // simulate a violated release invariant
        assert(service.CancelBlockedHandoff(h,76,ticket)==-5 && BdwFenceState(signal)==2);
        assert(h.policy.slots[ticket.slot].state==HandoffSlotState::Quarantined && h.buffers[ticket.slot]==output);
        assert(!BdwGpuBufferBeginGpuUse(output));_exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    // Feed a real i915 exec ioctl through fence publication, handoff routing,
    // blocked cancellation, frozen inspection and owner exit. Seed the wait
    // fence directly because userspace cannot create an unsignaled fence with
    // this backend without another pending GPU submission.
    {pid_t child=fork();assert(child>=0);if(!child){
        struct RawObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
        struct RawFence {uint32_t handle,flags;};
        struct RawExec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);assert(output);
        const auto waitId=BdwGpuFileCreateSync(f,false),signalId=BdwGpuFileCreateSync(f,false);assert(waitId && signalId);
        auto* waitSync=BdwGpuFileGetSync(f,waitId);auto* signalSync=BdwGpuFileGetSync(f,signalId);auto* dependency=BdwFenceCreate();
        assert(waitSync && signalSync && dependency && BdwGpuSyncReplace(waitSync,dependency));
        BdwHandoffSession session;FakeWorker host;BdwSubmissionWorker worker;assert(worker.Start(host.Hooks(),0,true));
        assert(session.Attach(80));const auto ticket=session.Reserve(80,output);assert(ticket>0);
        RawObject objects[]={{replacement,0,0,4096,0x20000,16,0,0},{protectedHandle,0,0,4096,0x10000,20,0,0}};
        RawFence fences[]={{waitId,1},{signalId,2}};
        RawExec request{reinterpret_cast<uint64_t>(objects),2,0,8,0,0,2,reinterpret_cast<uint64_t>(fences),
            (1u<<11)|(1u<<18)|(1u<<19),0,0};
        FakeUser state{};state.session=&session;state.handoffWorker=&worker;state.handoffOwner=80;
        BdwI915UserAccess access{&state,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        access.submit=UserSubmit;access.renderExperiment=true;access.handoffSubmit=UserSessionSubmit;
        access.submitterPid=42;access.submitterIncarnation=80;
        assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(request)),reinterpret_cast<uint64_t>(&request),access));
        auto* published=BdwGpuSyncAcquire(signalSync);assert(published && !BdwFenceState(published));
        assert(session.Observe(80,ticket)==0 && worker.Step()==BdwSubmissionStep::Idle && !host.executions);
        assert(!session.BeginBlockedRecovery(80,ticket,worker) && BdwFenceState(published)==2);
        BdwExecOwnerWork work{};assert(session.RecoveryWork(80,worker,work));
        assert(!work.queued && !work.active && !work.quarantined && !work.unknown && !work.retiring && !work.preparing);
        session.OwnerExit(80,worker);assert(session.Attach(81) && !worker.IsFrozenOwner(80));
        const auto next=session.Reserve(81,output);assert(next>ticket && !session.Release(81,next));
        session.Detach(81);_exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    // The session owns the recovery freeze through detach attempts and thaws
    // it on owner exit, allowing a replacement renderer to attach.
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* s=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* wait=BdwFenceCreate();auto* signal=BdwFenceCreate();assert(output && s && wait && signal);
        BdwExecSnapshotSetOrigin(s,123,0,76);
        assert(BdwExecSnapshotAddWaitFence(s,wait) && BdwExecSnapshotAddSignalFence(s,signal));
        BdwHandoffSession session;FakeWorker host;BdwSubmissionWorker worker;assert(worker.Start(host.Hooks(),0));
        assert(session.Attach(76));const auto ticket=session.Reserve(76,output);assert(ticket>0);
        int64_t submitted=-1;assert(session.Submit(76,worker,s,submitted) && !submitted);
        auto* extra=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* ready=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* foreign=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* extraWait=BdwFenceCreate();
        auto* extraSignal=BdwFenceCreate();auto* readySignal=BdwFenceCreate();
        assert(extra && ready && foreign && extraWait && extraSignal && readySignal);
        BdwExecSnapshotSetOrigin(extra,123,0,76);BdwExecSnapshotSetOrigin(ready,123,0,76);
        BdwExecSnapshotSetOrigin(foreign,124,0,77);
        assert(BdwExecSnapshotAddWaitFence(extra,extraWait) && BdwExecSnapshotAddSignalFence(extra,extraSignal));
        assert(BdwExecSnapshotAddSignalFence(ready,readySignal));
        assert(!worker.Submit(extra) && !worker.Submit(ready) && !worker.Submit(foreign));
        auto* output2=BdwGpuBufferCreateHandoff(4096);assert(output2);
        const auto handle2=BdwGpuFileImportBuffer(f,output2);assert(handle2);
        BdwExecObjectInput objects2[]={{replacement,0x20000,4096,16},{handle2,0x10000,4096,20}};
        auto* handoff2=BdwExecSnapshotCreate(f,objects2,2,0,8,64);auto* signal2=BdwFenceCreate();assert(handoff2 && signal2);
        BdwExecSnapshotSetOrigin(handoff2,123,0,76);assert(BdwExecSnapshotAddSignalFence(handoff2,signal2));
        const auto ticket2=session.Reserve(76,output2);assert(ticket2>ticket);
        assert(session.Submit(76,worker,handoff2,submitted) && !submitted);
        assert(session.BeginBlockedRecovery(77,ticket,worker)==-2);
        assert(!session.BeginBlockedRecovery(76,ticket,worker) && BdwFenceState(signal)==2);
        assert(BdwFenceState(extraSignal)==2 && BdwFenceState(readySignal)==2 && BdwFenceState(signal2)==2);
        const auto reused=session.Reserve(76,output2);assert(reused>ticket2 && !session.Release(76,reused));
        BdwExecOwnerWork work{};assert(session.RecoveryWork(76,worker,work) && !work.queued && !work.active && !work.unknown && !work.retiring && !work.preparing);
        assert(worker.Step()==BdwSubmissionStep::Finished); // foreign submission still runs
        assert(!session.RecoveryWork(77,worker,work) && !session.AbandonRecovery(77,worker));
        session.Detach(76);assert(session.OwnedBy(76) && !session.Attach(77));
        session.OwnerExit(77,worker);assert(worker.IsFrozenOwner(76));
        session.OwnerExit(76,worker);assert(!worker.IsFrozenOwner(76) && session.Attach(77));
        auto* replacement=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(replacement && !worker.Submit(replacement));
        assert(worker.Step()==BdwSubmissionStep::Finished);
        auto* abandoned=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);auto* pending=BdwFenceCreate();auto* completion=BdwFenceCreate();
        assert(abandoned && pending && completion);BdwExecSnapshotSetOrigin(abandoned,124,0,77);
        assert(BdwExecSnapshotAddWaitFence(abandoned,pending) && BdwExecSnapshotAddSignalFence(abandoned,completion));
        const auto next=session.Reserve(77,output);assert(next>ticket);
        assert(session.Submit(77,worker,abandoned,submitted) && !submitted && !session.BeginBlockedRecovery(77,next,worker));
        assert(session.AbandonRecovery(77,worker) && !worker.IsFrozenOwner(77));
        assert(session.Detach(77));_exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    // Production application worker rejects unknown origins even before a
    // freeze, and cannot misattribute a protected handoff to another owner.
    {
        BdwSubmissionQueue service;assert(service.Init(true));BdwSubmissionPreparation prep{};
        assert(service.BeginPreparation(0,prep)==-22 && !service.BeginPreparation(76,prep));service.EndPreparation(prep);
        auto* unknown=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(unknown && service.Submit(unknown)==-22);
        assert(BdwExecSnapshotRelease(unknown,false));
        auto* output=BdwGpuFileGetBuffer(f,protectedHandle);auto* s=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
        auto* signal=BdwFenceCreate();assert(output && s && signal && BdwExecSnapshotAddSignalFence(s,signal));
        BdwExecSnapshotSetOrigin(s,123,0,77);BdwHandoffResources h{};HandoffTicket ticket{};
        assert(HandoffAttach(h.policy,76) && BdwHandoffReserve(h,76,7,9,output,316,ticket));
        assert(service.SubmitHandoff(h,76,ticket,s,signal,316)==-22 && h.policy.slots[ticket.slot].state==HandoffSlotState::Reserved);
        assert(BdwExecSnapshotRelease(s,false) && BdwHandoffCancel(h,76,ticket));BdwHandoffDetach(h,76);
        BdwFenceUnref(signal);BdwGpuBufferUnref(output);
        BdwExecOwnerWork work{};assert(service.FreezeOwner(76,work));
        auto* other=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(other);BdwExecSnapshotSetOrigin(other,124,0,77);
        assert(!service.Submit(other));uint64_t id=0;BdwExecSnapshot* item=nullptr;
        assert(service.Take(id,item) && item==other && service.Finish(id,true,true) && service.ThawOwner(76));
    }
    // Freezing and draining queued work never cancels the active snapshot.
    {
        BdwSubmissionQueue service;assert(service.Init());
        auto* active=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* queued=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* foreign=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* completion=BdwFenceCreate();assert(active && queued && foreign && completion);
        BdwExecSnapshotSetOrigin(active,123,0,76);BdwExecSnapshotSetOrigin(queued,123,0,76);BdwExecSnapshotSetOrigin(foreign,124,0,77);
        assert(BdwExecSnapshotAddSignalFence(queued,completion));
        assert(!service.Submit(active) && !service.Submit(queued) && !service.Submit(foreign));
        uint64_t id=0;BdwExecSnapshot* item=nullptr;assert(service.Take(id,item) && item==active);
        BdwExecOwnerWork work{};assert(service.FreezeOwner(76,work) && work.active==1 && work.queued==1);
        BdwHandoffResources unused{};
        assert(service.CancelFrozenOwnerQueued(unused,77)==-2 && service.CancelFrozenOwnerQueued(unused,76)==1);
        assert(BdwFenceState(completion)==2 && service.FrozenOwnerWork(76,work) && work.active==1 && !work.queued);
        assert(service.Finish(id,true,true) && service.Take(id,item) && item==foreign && service.Finish(id,true,true));
        assert(service.FrozenOwnerWork(76,work) && !work.active && !work.queued && service.ThawOwner(76));
        BdwFenceUnref(completion);
    }
    // A second GPU file can share a buffer with the renderer. Closing the
    // renderer's file must not revoke either queued snapshot or its context,
    // and an owner-only drain must not be mistaken for a global buffer drain.
    {
        auto* shared=BdwGpuFileGetBuffer(f,replacement);auto* renderer=BdwGpuFileCreate();auto* peer=BdwGpuFileCreate();
        assert(shared && renderer && peer);
        auto rh=BdwGpuFileImportBuffer(renderer,shared),ph=BdwGpuFileImportBuffer(peer,shared);assert(rh && ph);
        BdwExecObjectInput ri{rh,0x10000,4096,16},pi{ph,0x10000,4096,16};
        auto* a=BdwExecSnapshotCreate(renderer,&ri,1,0,0,8);auto* b=BdwExecSnapshotCreate(peer,&pi,1,0,0,8);
        auto* context=BdwGpuFileGetDefaultContext(renderer);assert(a && b && context && BdwExecSnapshotBindContext(a,context));
        BdwGpuContextUnref(context);BdwExecSnapshotSetOrigin(a,123,0,76);BdwExecSnapshotSetOrigin(b,124,0,77);
        BdwSubmissionQueue service;assert(service.Init() && !service.Submit(a) && !service.Submit(b));
        BdwGpuFileUnref(renderer);assert(BdwExecSnapshotContext(a) && BdwGpuBufferGpuUses(shared)==2);
        BdwExecOwnerWork work{};assert(service.FreezeOwner(76,work) && work.queued==1);
        uint64_t id=0;BdwExecSnapshot* item=nullptr;assert(service.Take(id,item) && item==a && service.Finish(id,true,true));
        assert(service.FrozenOwnerWork(76,work) && !work.queued && !work.active && !work.retiring && !work.unknown);
        assert(BdwGpuBufferGpuUses(shared)==1); // peer work still owns the same storage
        assert(service.Take(id,item) && item==b && service.Finish(id,true,true));
        assert(!BdwGpuBufferGpuUses(shared) && service.ThawOwner(76));
        BdwGpuFileUnref(peer);BdwGpuBufferUnref(shared);
    }
    // Retained references remain visible between reaping and actual release,
    // including unknown-origin work and overlapping releases by other owners.
    {
        BdwRetirementLedger ledger;BdwRetirementLedger::Node same{},other{},unknown{};
        ledger.Add(same,76);ledger.Add(other,77);ledger.Add(unknown,0);
        BdwExecOwnerWork work{};ledger.Count(76,work);assert(work.retiring==1 && work.unknown==1);
        assert(ledger.Remove(other) && ledger.Remove(same));work={};ledger.Count(76,work);
        assert(!work.retiring && work.unknown==1 && ledger.Remove(unknown));
        assert(!ledger.Remove(unknown));
    }
    // Exercise the real ioctl guard while user copying is stalled, before a
    // snapshot exists. Freeze must see it and reject a later ioctl.
    {
        struct PrepFixture {BdwSubmissionQueue* queue;std::atomic<bool> entered{false},resume{false};};
        BdwSubmissionQueue service;assert(service.Init());PrepFixture fixture{&service};
        BdwI915UserAccess user{};user.context=&fixture;user.submitterIncarnation=76;
        user.read=[](void* c,void*,uint64_t,uint64_t){auto& f=*static_cast<PrepFixture*>(c);
            f.entered.store(true,std::memory_order_release);while(!f.resume.load(std::memory_order_acquire)){} return false;};
        user.writable=[](void*,uint64_t,uint64_t){return true;};
        user.write=[](void*,uint64_t,const void*,uint64_t){return true;};
        user.submit=[](void*,BdwExecSnapshot*)->int64_t{return -16;};
        user.beginExec=[](void* c,uint64_t owner,BdwSubmissionPreparation& entry)->int64_t{
            return static_cast<PrepFixture*>(c)->queue->BeginPreparation(owner,entry);};
        user.endExec=[](void* c,BdwSubmissionPreparation& entry){static_cast<PrepFixture*>(c)->queue->EndPreparation(entry);};
        BdwSubmissionPreparation priorUnknown{};assert(!service.BeginPreparation(0,priorUnknown));
        int64_t result=0;std::thread first([&]{result=BdwIntelRenderIoctl(f,ioctlCommand(0x69,64),1,user);});
        while(!fixture.entered.load(std::memory_order_acquire)){}
        BdwExecOwnerWork work{};assert(service.FreezeOwner(76,work) && work.preparing==1 && work.unknown==1);
        assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,64),1,user)==-16);
        BdwSubmissionPreparation unknown{},other{};assert(service.BeginPreparation(0,unknown)==-16);
        assert(!service.BeginPreparation(77,other));
        assert(service.FrozenOwnerWork(76,work) && work.preparing==1 && work.unknown==1);
        service.EndPreparation(other);service.EndPreparation(priorUnknown);
        fixture.resume.store(true,std::memory_order_release);first.join();
        assert(result==-14 && service.FrozenOwnerWork(76,work) && !work.preparing && service.ThawOwner(76));
    }
    // Cancellation and the admission freeze must be one atomic boundary:
    // concurrent work is either visible in the frozen snapshot or refused.
    for(unsigned iteration=0;iteration<32;++iteration){
        pid_t child=fork();assert(child>=0);if(!child){
            auto* output=BdwGpuFileGetBuffer(f,protectedHandle);
            auto* blocked=BdwExecSnapshotCreate(f,protectedExec,2,0,8,64);
            auto* next=BdwExecSnapshotCreate(f,exec,2,0,8,64);
            auto* wait=BdwFenceCreate();auto* signal=BdwFenceCreate();
            assert(output && blocked && next && wait && signal);
            BdwExecSnapshotSetOrigin(blocked,123,0,76);BdwExecSnapshotSetOrigin(next,123,0,76);
            assert(BdwExecSnapshotAddWaitFence(blocked,wait) && BdwExecSnapshotAddSignalFence(blocked,signal));
            BdwHandoffResources h{};HandoffTicket ticket{};BdwSubmissionQueue service;assert(service.Init());
            assert(HandoffAttach(h.policy,76) && BdwHandoffReserve(h,76,7,9,output,314,ticket));
            assert(!service.SubmitHandoff(h,76,ticket,blocked,signal,314));
            std::atomic<bool> go{false};int64_t cancel=-99,submit=-99;
            std::thread a([&]{while(!go.load(std::memory_order_acquire)){} cancel=service.FreezeAndCancelBlockedHandoff(h,76,ticket);});
            std::thread b([&]{while(!go.load(std::memory_order_acquire)){} submit=service.Submit(next);});
            go.store(true,std::memory_order_release);a.join();b.join();
            assert(!cancel && (submit==0 || submit==-16));
            BdwExecOwnerWork work{};assert(service.FrozenOwnerWork(76,work));
            assert(work.queued==(submit==0?1u:0u) && !work.active && !work.quarantined && !work.unknown && !work.retiring);
            if(!submit){uint64_t id=0;BdwExecSnapshot* item=nullptr;assert(service.Take(id,item) && item==next && service.Finish(id,true,true));}
            else assert(BdwExecSnapshotRelease(next,false));
            assert(h.policy.slots[ticket.slot].state==HandoffSlotState::Free);
            _exit(0);
        }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    assert(BdwGpuFileCloseBuffer(f,protectedHandle) && BdwGpuFileCloseBuffer(other,protectedAlias));
    BdwGpuBufferUnref(protectedOutput);
    {
        auto* buffer=BdwGpuFileGetBuffer(f,next);assert(buffer);
        auto* p0=reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(buffer,0));
        auto* p1=reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(buffer,1));assert(p0 && p1);
        for(unsigned i=0;i<8;++i)p0[4088+i]=uint8_t(i+1);
        for(unsigned i=0;i<24;++i)p1[i]=uint8_t(i+9);
        BdwExecObjectInput input{next,0x10000,4096,16};
        auto* sampleSnapshot=BdwExecSnapshotCreate(f,&input,1,0,4088,19);assert(sampleSnapshot);
        auto sample=BdwExecSnapshotBatchSample(sampleSnapshot);
        assert(sample.bytes==19 && sample.words[0]==0x04030201 && sample.words[1]==0x08070605 &&
               sample.words[2]==0x0c0b0a09 && sample.words[4]==0x00131211 && !sample.words[5]);
        p0[4088]=0xff;p1[0]=0xff;auto retained=BdwExecSnapshotBatchSample(sampleSnapshot);
        assert(retained.words[0]==sample.words[0] && retained.words[2]==sample.words[2]);
        auto current=BdwExecSnapshotCurrentBatchSample(sampleSnapshot);
        assert(current.bytes==19 && current.words[0]==0x040302ff && current.words[2]==0x0c0b0aff);
        assert(!BdwExecSnapshotCurrentBatchSample(nullptr).bytes);
        assert(BdwExecSnapshotRelease(sampleSnapshot,false));BdwGpuBufferUnref(buffer);
    }
    {
        auto* buffer=BdwGpuFileGetBuffer(f,replacement);assert(buffer);
        auto* data=reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(buffer,0));assert(data);
        data[0]=0x43;data[127]=0x9a;
        BdwExecObjectInput inputs[]={{replacement,0x20000,4096,16|128},{next,0x10000,4096,16|4}};
        auto* snapshot=BdwExecSnapshotCreate(f,inputs,2,0,0,8);assert(snapshot);
        data[0]=0xa5;BdwGpuCapture capture{};BdwExecSnapshotCapture(snapshot,capture);
        assert(capture.version==4 && capture.requested==1 && capture.count==1 && capture.entries[0].address==0x20000 &&
               capture.entries[0].size==4096 && capture.entries[0].bytes==4096 && capture.entries[0].data[0]==0xa5 && capture.entries[0].data[127]==0x9a);
        assert(BdwExecSnapshotRelease(snapshot,false));BdwGpuBufferUnref(buffer);
    }
    {
        auto* buffer=BdwGpuFileGetBuffer(f,next);assert(buffer);
        auto* p0=reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(buffer,0));
        auto* p1=reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(buffer,1));assert(p0 && p1);
        p0[4095]=0x3c;p1[0]=0x5a;p1[4095]=0xa7;
        BdwExecObjectInput input{next,0x10000,4096,16|128};
        auto* snapshot=BdwExecSnapshotCreate(f,&input,1,0,0,8);assert(snapshot);
        BdwGpuCapture capture{};BdwExecSnapshotCapture(snapshot,capture);
        assert(capture.count==1 && capture.entries[0].bytes==8192 && capture.entries[0].data[4095]==0x3c &&
               capture.entries[0].data[4096]==0x5a && capture.entries[0].data[8191]==0xa7);
        assert(BdwExecSnapshotRelease(snapshot,false));BdwGpuBufferUnref(buffer);
    }
    {
        // A transferred timeline point waits for both the original destination
        // point and the immutable source, even after either object is reset.
        BdwTimelineSync timeline;assert(timeline.Init());
        auto* prior=timeline.Prepare(1);assert(prior && timeline.Publish(prior));
        auto* source=BdwFenceCreate();assert(source);
        auto* joined=timeline.Prepare(2,source);assert(joined && BdwFenceComplete(joined,true) && timeline.Publish(joined));
        assert(!BdwFenceState(joined) && !timeline.Query(false) && timeline.Query(true)==2);
        assert(BdwFenceComplete(source,true) && !BdwFenceState(joined));
        assert(BdwFenceComplete(prior,true) && BdwFenceState(joined)==1 && timeline.Query(false)==2);
        timeline.Close();BdwFenceUnref(prior);BdwFenceUnref(source);BdwFenceUnref(joined);
        BdwTimelineSync failures;assert(failures.Init());source=BdwFenceCreate();assert(source);
        joined=failures.Prepare(7,source);assert(joined && BdwFenceComplete(joined,true) && failures.Publish(joined));
        assert(BdwFenceComplete(source,false) && BdwFenceState(joined)==2 && failures.Query(false)==7);
        failures.Close();BdwFenceUnref(source);BdwFenceUnref(joined);
        auto* leaf=BdwFenceCreate();assert(leaf);BdwFenceRef(leaf);auto* chain=leaf;
        for(unsigned i=0;i<256;++i){auto* nextFence=BdwFenceCreate(true,nullptr,0,chain);assert(nextFence);BdwFenceUnref(chain);chain=nextFence;}
        assert(!BdwFenceState(chain));assert(BdwFenceComplete(leaf,true));
        failHeapOnce=true;assert(!BdwFenceState(chain) && !failHeapOnce);
        assert(BdwFenceState(chain)==1);BdwFenceUnref(chain);BdwFenceUnref(leaf);
        leaf=BdwFenceCreate();assert(leaf);BdwFenceRef(leaf);chain=leaf;
        for(unsigned i=0;i<128;++i){auto* prior=BdwFenceCreate(true);assert(prior);
            auto* nextFence=BdwFenceCreate(true,prior,0,chain);assert(nextFence);
            BdwFenceUnref(prior);BdwFenceUnref(chain);chain=nextFence;}
        assert(!BdwFenceState(chain));assert(BdwFenceComplete(leaf,false));
        assert(BdwFenceState(chain)==2);BdwFenceUnref(chain);BdwFenceUnref(leaf);
        // Each node shares both edges with the prior node. Checking a finished
        // graph must reuse resolved subgraphs rather than visit 2^80 paths.
        chain=BdwFenceCreate(true);assert(chain);
        for(unsigned i=0;i<80;++i){auto* nextFence=BdwFenceCreate(true,chain,0,chain);assert(nextFence);
            BdwFenceUnref(chain);chain=nextFence;}
        assert(BdwFenceState(chain)==1);BdwFenceUnref(chain);
    }
    {
        // A later timeline point cannot complete while its predecessor is
        // pending, even if the later GPU operation retires first.
        auto* first=BdwFenceCreate();auto* later=BdwFenceCreate(false,first);assert(first && later);
        assert(BdwFenceComplete(later,true) && BdwFenceState(later)==0);
        assert(BdwFenceComplete(first,true) && BdwFenceState(later)==1);
        BdwFenceUnref(first);BdwFenceUnref(later);
        first=BdwFenceCreate();later=BdwFenceCreate(false,first);assert(first && later);
        assert(BdwFenceComplete(first,false) && BdwFenceState(later)==0);
        assert(BdwFenceComplete(later,true) && BdwFenceState(later)==2);
        BdwFenceUnref(first);BdwFenceUnref(later);
    }
    {
        // Lookup returns the earliest submitted point satisfying a wait;
        // a racing preparation must not overwrite the published chain.
        BdwTimelineSync timeline;assert(timeline.Init());
        auto* five=timeline.Prepare(5);auto* stale=timeline.Prepare(7);assert(five && stale);
        assert(timeline.Publish(five) && !timeline.Publish(stale));BdwFenceUnref(stale);
        auto* ten=timeline.Prepare(10);assert(ten && timeline.Publish(ten));
        assert(timeline.Query(true)==10 && timeline.Query(false)==0);
        auto* current=timeline.Find(0);assert(current==ten);BdwFenceUnref(current);
        assert(!timeline.Find(11));
        auto* found=timeline.Find(4);assert(found==five);BdwFenceUnref(found);
        found=timeline.Find(6);assert(found==ten);BdwFenceUnref(found);
        assert(BdwFenceComplete(ten,true) && BdwFenceState(ten)==0);
        assert(timeline.Query(false)==0);
        assert(BdwFenceComplete(five,true) && BdwFenceState(ten)==1 && timeline.Query(false)==10);
        auto* eleven=timeline.Prepare(11);assert(eleven && timeline.Publish(eleven));
        found=timeline.Find(4);assert(found && BdwFenceState(found)==1);BdwFenceUnref(found);
        BdwFenceUnref(eleven);
        timeline.Close();assert(BdwFenceState(ten)==1);BdwFenceUnref(five);BdwFenceUnref(ten);
    }
    {
        // An out-of-order insertion starts a new chain context; it must not
        // resolve a wait using the older fence even if its point is larger.
        BdwTimelineSync timeline;assert(timeline.Init());
        auto* old=timeline.Prepare(9);assert(old && timeline.Publish(old));
        auto* next=timeline.Prepare(3);assert(next && next->point==9 && timeline.Publish(next));
        auto* found=timeline.Find(3);assert(found==next);BdwFenceUnref(found);
        assert(BdwFenceComplete(old,true) && timeline.Query(false)==0 && !BdwFenceState(next));
        assert(BdwFenceComplete(next,true) && timeline.Query(false)==9);
        timeline.Close();BdwFenceUnref(old);BdwFenceUnref(next);
    }
    {
        // Destroying a sync handle does not revoke timeline references held
        // by an already admitted operation or a caller retaining the object.
        const auto id=BdwGpuFileCreateSync(f,false);assert(id);
        auto* sync=BdwGpuFileGetSync(f,id);assert(sync);
        const auto alias=BdwGpuFileImportSync(f,sync);assert(alias && alias!=id);
        auto* point=BdwGpuSyncPrepareTimeline(sync,9);assert(point && BdwGpuSyncPublishTimeline(sync,point));
        auto* binary=BdwGpuSyncAcquire(sync);assert(binary==point);BdwFenceUnref(binary);
        assert(BdwGpuFileDestroySync(f,id) && !BdwGpuFileGetSync(f,id));
        auto* aliasRef=BdwGpuFileGetSync(f,alias);assert(aliasRef==sync);BdwGpuSyncUnref(aliasRef);
        assert(BdwGpuFileDestroySync(f,alias));
        auto* held=BdwGpuSyncFindTimeline(sync,9);assert(held==point);
        BdwGpuSyncUnref(sync);assert(BdwFenceComplete(point,true) && BdwFenceState(held)==1);
        BdwFenceUnref(point);BdwFenceUnref(held);
    }
    {
        const auto id=BdwGpuFileCreateSync(f,true);auto* sync=BdwGpuFileGetSync(f,id);assert(sync);
        auto* point=BdwGpuSyncPrepareTimeline(sync,3);assert(point && BdwGpuSyncPublishTimeline(sync,point));
        auto* binary=BdwGpuSyncAcquire(sync);assert(binary==point);BdwFenceUnref(binary);
        assert(BdwGpuSyncReplace(sync,nullptr) && !BdwGpuSyncAcquire(sync) && !BdwGpuSyncFindTimeline(sync,3));
        BdwFenceUnref(point);BdwGpuSyncUnref(sync);assert(BdwGpuFileDestroySync(f,id));
    }
    {
        // The exported snapshot joins existing producer work only. A later
        // consumer waiting on it must not become its own dependency.
        auto* buffer=BdwGpuFileGetBuffer(f,next);assert(buffer);
        BdwExecQueue queue;auto* idle=queue.AcquireBufferFence(buffer);assert(idle && BdwFenceState(idle)==1);BdwFenceUnref(idle);
        auto* a=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* b=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* consumer=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(a && b && consumer);
        uint64_t aid=0,bid=0,cid=0,current=0;BdwExecSnapshot* item=nullptr;
        assert(queue.Enqueue(1,a,aid) && queue.Enqueue(1,b,bid));
        auto* fence=queue.AcquireBufferFence(buffer);assert(fence && !BdwFenceState(fence));
        assert(BdwExecSnapshotAddWaitFence(consumer,fence) && queue.Enqueue(1,consumer,cid));
        assert(queue.StartNext(current,item) && current==aid && queue.Finish(aid,true,true));
        assert(!BdwFenceState(fence));
        assert(queue.StartNext(current,item) && current==bid && queue.Finish(bid,true,true));
        assert(BdwFenceState(fence)==1);
        assert(queue.StartNext(current,item) && current==cid && queue.Finish(cid,true,true));
        BdwExecRequestState state{};
        assert(queue.Reap(1,aid,state)==a && BdwExecSnapshotRelease(a,false));
        assert(queue.Reap(1,bid,state)==b && BdwExecSnapshotRelease(b,false));
        assert(queue.Reap(1,cid,state)==consumer && BdwExecSnapshotRelease(consumer,false));
        BdwFenceUnref(fence);
        assert(BdwGpuBufferBeginGpuUse(buffer) && !queue.AcquireBufferFence(buffer));
        assert(BdwGpuBufferEndGpuUse(buffer));
        auto* cancelled=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(cancelled);
        uint64_t cancelledId=0;assert(queue.Enqueue(1,cancelled,cancelledId));
        auto* failed=queue.AcquireBufferFence(buffer);assert(failed && !BdwFenceState(failed));
        assert(queue.Cancel(1,cancelledId) && BdwFenceState(failed)==2);
        assert(queue.Reap(1,cancelledId,state)==cancelled && BdwExecSnapshotRelease(cancelled,false));
        BdwFenceUnref(failed);BdwGpuBufferUnref(buffer);
    }
    {
        auto* buffer=BdwGpuFileGetBuffer(f,next);assert(buffer);
        BdwSubmissionQueue queue;BdwSubmissionPreparation preparing{};
        assert(queue.Init(true) && !queue.BeginPreparation(7,preparing));
        assert(!queue.AcquireBufferFence(buffer));
        queue.EndPreparation(preparing);
        auto* fence=queue.AcquireBufferFence(buffer);assert(fence && BdwFenceState(fence)==1);
        BdwFenceUnref(fence);BdwGpuBufferUnref(buffer);
    }
    {
        BdwBinarySync sync;assert(sync.Init() && !sync.Acquire());
        auto* a=BdwFenceCreate();auto* b=BdwFenceCreate();assert(a && b && sync.Replace(a));
        auto* snapshot=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(snapshot);
        assert(BdwExecSnapshotObjectCount(snapshot)==2 && BdwExecSnapshotPageCount(snapshot)==3 &&
               BdwExecSnapshotBatchAddress(snapshot)==0x20008 && BdwExecSnapshotBatchLength(snapshot)==64);
        assert(BdwExecSnapshotAddSignalFence(snapshot,a) && !BdwExecSnapshotAddSignalFence(snapshot,a));
        BdwExecQueue queue;uint64_t id=0,active=0;BdwExecSnapshot* item=nullptr;
        assert(queue.Enqueue(1,snapshot,id) && sync.Replace(b));
        auto* current=sync.Acquire();assert(current==b && !BdwFenceState(current));BdwFenceUnref(current);
        assert(queue.StartNext(active,item) && active==id && queue.Finish(id,true,true));
        assert(BdwFenceState(a)==1 && BdwFenceState(b)==0 && !BdwFenceComplete(a,true));
        BdwExecRequestState state{};assert(queue.Reap(1,id,state)==snapshot && BdwExecSnapshotRelease(snapshot,false));
        assert(sync.Replace(nullptr) && !sync.Acquire());sync.Close();BdwFenceUnref(a);BdwFenceUnref(b);
    }
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* a=BdwFenceCreate();auto* b=BdwFenceCreate();assert(a && b);
        auto* first=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* second=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(first && second);
        assert(BdwExecSnapshotAddSignalFence(first,a) && BdwExecSnapshotAddSignalFence(second,b));
        BdwExecQueue queue;uint64_t one=0,two=0,active=0;BdwExecSnapshot* item=nullptr;
        assert(queue.Enqueue(1,first,one) && queue.Enqueue(1,second,two) && queue.StartNext(active,item));
        auto* buffer=BdwGpuFileGetBuffer(f,next);assert(buffer);
        auto* joined=queue.AcquireBufferFence(buffer);assert(joined && !BdwFenceState(joined));
        assert(queue.Finish(one,false,false) && queue.Poisoned());
        assert(!BdwFenceState(joined) && !queue.AcquireBufferFence(buffer));
        assert(BdwFenceState(a)==0 && BdwFenceState(b)==2); // uncertain active work stays pending
        BdwExecRequestState state{};assert(queue.Reap(1,two,state)==second && BdwExecSnapshotRelease(second,false));
        // Active quarantined snapshot intentionally remains owned until recovery.
        BdwFenceUnref(joined);BdwGpuBufferUnref(buffer);BdwFenceUnref(a);BdwFenceUnref(b);_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    {
        auto* batch=BdwGpuFileGetBuffer(f,replacement);assert(batch);
        auto* words=reinterpret_cast<uint32_t*>(BdwGpuBufferPageKernelAddress(batch,0));assert(words);
        words[0]=0x10000002;words[1]=0x10000;words[2]=0;words[3]=0x12345678;words[4]=0x05000000;words[5]=0;
        auto* s=BdwExecSnapshotCreate(f,exec,2,0,0,24);assert(s);
        words[0]=0x11000002;assert(!BdwExecSnapshotShadowStoreProbe(s));words[0]=0x10000002;
        words[1]=0x20000;assert(!BdwExecSnapshotShadowStoreProbe(s));words[1]=0x10001;
        assert(!BdwExecSnapshotShadowStoreProbe(s));words[1]=0x10000;
        words[2]=1;assert(!BdwExecSnapshotShadowStoreProbe(s));words[2]=0;
        words[5]=1;assert(!BdwExecSnapshotShadowStoreProbe(s));words[5]=0;
        assert(BdwExecSnapshotShadowStoreProbe(s) && !BdwExecSnapshotShadowStoreProbe(s));
        auto* shadow=reinterpret_cast<const uint32_t*>(BdwExecSnapshotTestBatchKernelAddress(s));
        assert(shadow && shadow!=words && shadow[0]==0x10000002 && shadow[1]==0x10000 && shadow[3]==0x12345678);
        auto batchSample=BdwExecSnapshotBatchSample(s);
        assert(batchSample.bytes==24 && batchSample.words[0]==shadow[0] && batchSample.words[3]==shadow[3]);
        words[0]=0xdeadbeef;words[1]=0xfeed0000;words[3]=0;
        assert(shadow[0]==0x10000002 && shadow[1]==0x10000 && shadow[3]==0x12345678);
        assert(BdwExecSnapshotRelease(s,false));BdwGpuBufferUnref(batch);
    }
    {
        struct ContextWorker {
            unsigned calls=0;
            static bool Launch(void*,void (*)(void*),void*,uint32_t){return true;}
            static void Sleep(void*){}
            static void Recover(void*){assert(false);}
            static BdwSubmissionOutcome Execute(void* opaque,BdwExecSnapshot* snapshot){
                auto& self=*static_cast<ContextWorker*>(opaque);static uint32_t image[20*1024];bool present=false;
                auto* context=BdwExecSnapshotContext(snapshot);assert(context && BdwGpuContextBegin(context,image,present));
                if(self.calls==0){assert(!present);image[0]=11;}
                else if(self.calls==1){assert(!present);image[0]=22;}
                else {assert(present && image[0]==11);image[0]=33;}
                ++self.calls;assert(BdwGpuContextFinish(context,&image,true));return {true,true};
            }
        } host;
        auto idA=BdwGpuFileCreateContext(f),idB=BdwGpuFileCreateContext(f);assert(idA && idB);
        auto* a=BdwGpuFileGetContext(f,idA);auto* b=BdwGpuFileGetContext(f,idB);assert(a && b);
        BdwSubmissionWorker worker;BdwSubmissionWorker::Hooks hooks{&host,ContextWorker::Launch,ContextWorker::Sleep,ContextWorker::Execute,ContextWorker::Recover};
        assert(worker.Start(hooks,0));
        for(unsigned i=0;i<3;++i){auto* snapshot=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(snapshot);
            assert(BdwExecSnapshotBindContext(snapshot,i==1?b:a) && worker.Submit(snapshot)==0);}
        assert(BdwGpuFileDestroyContext(f,idA) && BdwGpuFileDestroyContext(f,idB));
        BdwGpuContextUnref(a);BdwGpuContextUnref(b);
        assert(worker.Step()==BdwSubmissionStep::Finished && worker.Step()==BdwSubmissionStep::Finished &&
               worker.Step()==BdwSubmissionStep::Finished && host.calls==3);
    }
    {
        const pid_t child=fork();assert(child>=0);
        if(!child){
            struct ThreadHost {
                std::atomic<unsigned> executed{0};
                static bool Launch(void*,void (*entry)(void*),void* arg,uint32_t cpu){
                    if(cpu!=0)return false;std::thread([=](){entry(arg);}).detach();return true;}
                static void Sleep(void*){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
                static BdwSubmissionOutcome Execute(void* c,BdwExecSnapshot*){
                    static_cast<ThreadHost*>(c)->executed.fetch_add(1,std::memory_order_release);return {true,true};}
                static void Recover(void*){_exit(2);}
            } host;
            BdwSubmissionWorker worker;BdwSubmissionWorker::Hooks hooks{&host,ThreadHost::Launch,ThreadHost::Sleep,ThreadHost::Execute,ThreadHost::Recover};
            auto* first=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* second=BdwExecSnapshotCreate(f,exec,2,0,8,64);
            auto* watched=BdwGpuFileGetBuffer(f,replacement);
            assert(first && second && watched && worker.Start(hooks,0) && worker.Submit(first)==0 && worker.Submit(second)==0);
            for(unsigned i=0;i<5000 && (host.executed.load(std::memory_order_acquire)!=2 || BdwGpuBufferGpuUses(watched));++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            assert(host.executed.load(std::memory_order_acquire)==2 && !BdwGpuBufferGpuUses(watched));
            _exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    {
        FakeWorker host;BdwSubmissionWorker worker;auto* first=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* second=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(first && second);
        assert(worker.Submit(first)==-19 && worker.Start(host.Hooks(),0) && host.entry && host.argument==&worker && host.cpu==0);
        assert(!worker.Start(host.Hooks(),0) && worker.Submit(first)==0 && worker.Submit(second)==0);
        assert(worker.Step()==BdwSubmissionStep::Finished && worker.Step()==BdwSubmissionStep::Finished && host.executions==2);
        assert(worker.Step()==BdwSubmissionStep::Idle);
        FakeWorker failedHost;failedHost.launchOk=false;BdwSubmissionWorker failedWorker;
        assert(!failedWorker.Start(failedHost.Hooks(),0));
        auto* rejected=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(rejected);
        assert(failedWorker.Submit(rejected)==-19 && BdwExecSnapshotRelease(rejected,false));
    }
    {
        FakeWorker host;BdwSubmissionWorker worker;auto* wait=BdwFenceCreate();auto* done=BdwFenceCreate();
        auto* snapshot=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(wait && done && snapshot);
        assert(BdwExecSnapshotAddWaitFence(snapshot,wait) && BdwExecSnapshotAddWaitFence(snapshot,wait));
        assert(BdwExecSnapshotAddSignalFence(snapshot,done) && worker.Start(host.Hooks(),0) && !worker.Submit(snapshot));
        assert(worker.Step()==BdwSubmissionStep::Idle && !host.executions && !BdwFenceState(done));
        assert(BdwFenceComplete(wait,true) && worker.Step()==BdwSubmissionStep::Finished);
        assert(host.executions==1 && BdwFenceState(done)==1);BdwFenceUnref(wait);BdwFenceUnref(done);
    }
    {
        auto* wait=BdwFenceCreate();auto* done=BdwFenceCreate();assert(wait && done);
        auto* blocked=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* ready=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(blocked && ready);
        assert(BdwExecSnapshotAddWaitFence(blocked,wait) && BdwExecSnapshotAddSignalFence(blocked,done));
        BdwExecQueue queue;uint64_t first=0,second=0,active=0;BdwExecSnapshot* item=nullptr;
        assert(queue.Enqueue(7,blocked,first) && queue.Enqueue(8,ready,second));
        assert(!queue.StartNext(active,item) && !queue.CancelBlocked(8,first) && !queue.CancelBlocked(8,second));
        assert(queue.CancelBlocked(7,first) && BdwFenceState(done)==2);
        assert(!queue.CancelBlocked(7,first) && queue.StartNext(active,item) && active==second && item==ready);
        assert(!queue.CancelBlocked(8,second) && queue.Finish(second,true,true));
        BdwExecRequestState state{};
        assert(queue.Reap(7,first,state)==blocked && state==BdwExecRequestState::Cancelled && BdwExecSnapshotRelease(blocked,false));
        assert(queue.Reap(8,second,state)==ready && state==BdwExecRequestState::Complete && BdwExecSnapshotRelease(ready,false));
        BdwFenceUnref(wait);BdwFenceUnref(done);
    }
    {pid_t child=fork();assert(child>=0);if(!child){
        auto* wait=BdwFenceCreate();auto* done=BdwFenceCreate();auto* s=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        assert(wait && done && s && BdwExecSnapshotAddWaitFence(s,wait) && BdwExecSnapshotAddSignalFence(s,done));
        BdwExecQueue queue;uint64_t id=0,active=0;BdwExecSnapshot* item=nullptr;
        assert(queue.Enqueue(7,s,id) && BdwFenceComplete(done,true));
        assert(!queue.CancelBlocked(7,id) && queue.Poisoned() && !queue.StartNext(active,item));
        BdwExecRequestView view{};assert(queue.Observe(7,id,view) && view.state==BdwExecRequestState::Quarantined);
        BdwExecRequestState state{};assert(!queue.Reap(7,id,state));_exit(0);
    }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
    {pid_t child=fork();assert(child>=0);if(!child){
        FakeWorker host;BdwSubmissionWorker worker;auto* wait=BdwFenceCreate();auto* done=BdwFenceCreate();
        auto* snapshot=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(wait && done && snapshot);
        assert(BdwExecSnapshotAddWaitFence(snapshot,wait) && BdwExecSnapshotAddSignalFence(snapshot,done));
        assert(worker.Start(host.Hooks(),0) && !worker.Submit(snapshot) && BdwFenceComplete(wait,false));
        assert(worker.Step()==BdwSubmissionStep::Finished && !host.executions && BdwFenceState(done)==2);
        BdwFenceUnref(wait);BdwFenceUnref(done);_exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    {
        BdwSubmissionQueue service;assert(service.Init() && !service.Init());
        auto* first=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* second=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(first && second);
        auto* watched=BdwGpuFileGetBuffer(f,replacement);assert(watched);
        assert(service.Submit(first)==0 && service.Submit(second)==0 && BdwGpuBufferGpuUses(watched)==2);
        uint64_t id=0;BdwExecSnapshot* item=nullptr;
        assert(service.Take(id,item) && item==first && !service.Take(id,item));
        assert(service.Finish(id,true,true) && BdwGpuBufferGpuUses(watched)==1);
        assert(service.Take(id,item) && item==second && service.Finish(id,true,true));
        assert(!BdwGpuBufferGpuUses(watched) && !service.Take(id,item));BdwGpuBufferUnref(watched);
    }
    {
        BdwSubmissionQueue service;assert(service.Init());
        auto* first=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* reused=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* unknown=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(first && reused && unknown);
        BdwExecSnapshotSetOrigin(first,123,0,100);BdwExecSnapshotSetOrigin(reused,123,0,101);
        assert(!service.Submit(first) && !service.Submit(reused) && !service.Submit(unknown));
        BdwExecOwnerWork work{};assert(!service.InspectOwner(0,work));
        assert(service.InspectOwner(100,work) && work.queued==1 && !work.active && work.unknown==1);
        assert(service.InspectOwner(101,work) && work.queued==1 && work.unknown==1);
        uint64_t id=0;BdwExecSnapshot* item=nullptr;assert(service.Take(id,item) && item==first);
        assert(service.InspectOwner(100,work) && !work.queued && work.active==1 && work.unknown==1);
        assert(service.Finish(id,true,true) && service.InspectOwner(100,work) && !work.queued && !work.active && work.unknown==1);
        assert(service.Take(id,item) && item==reused && service.Finish(id,true,true));
        assert(service.Take(id,item) && item==unknown && service.Finish(id,true,true));
        assert(service.InspectOwner(100,work) && !work.queued && !work.active && !work.unknown);
    }
    {
        BdwSubmissionQueue service;assert(service.Init());BdwExecOwnerWork work{};
        auto* existing=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(existing);
        BdwExecSnapshotSetOrigin(existing,123,0,100);assert(!service.Submit(existing));
        assert(service.FreezeOwner(100,work) && work.queued==1 && !service.FreezeOwner(101,work));
        auto* blocked=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* unknown=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* other=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(blocked && unknown && other);
        BdwExecSnapshotSetOrigin(blocked,123,0,100);BdwExecSnapshotSetOrigin(other,123,0,101);
        assert(service.Submit(blocked)==-16 && service.Submit(unknown)==-16 && !service.Submit(other));
        assert(BdwExecSnapshotRelease(blocked,false) && BdwExecSnapshotRelease(unknown,false));
        assert(!service.ThawOwner(101) && service.ThawOwner(100) && !service.ThawOwner(100));
        uint64_t id=0;BdwExecSnapshot* item=nullptr;
        assert(service.Take(id,item) && item==existing && service.Finish(id,true,true));
        assert(service.Take(id,item) && item==other && service.Finish(id,true,true));
    }
    for(unsigned iteration=0;iteration<32;++iteration){
        BdwSubmissionQueue service;assert(service.Init());auto* s=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(s);
        BdwExecSnapshotSetOrigin(s,123,0,100);std::atomic<bool> go{false};int64_t submitted=-99;bool frozen=false;BdwExecOwnerWork work{};
        std::thread submit([&]{while(!go.load(std::memory_order_acquire))std::this_thread::yield();submitted=service.Submit(s);});
        std::thread freeze([&]{while(!go.load(std::memory_order_acquire))std::this_thread::yield();frozen=service.FreezeOwner(100,work);});
        go.store(true,std::memory_order_release);submit.join();freeze.join();assert(frozen);
        if(!submitted){assert(work.queued==1);uint64_t id=0;BdwExecSnapshot* item=nullptr;
            assert(service.Take(id,item) && item==s && service.Finish(id,true,true));}
        else {assert(submitted==-16 && !work.queued && BdwExecSnapshotRelease(s,false));}
        assert(service.ThawOwner(100));
    }
    {
        BdwExecQueue full;BdwExecSnapshot* snapshots[32]{};uint64_t ids[32]{},unused=99;
        for(unsigned i=0;i<32;++i){snapshots[i]=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(snapshots[i] && full.Enqueue(1,snapshots[i],ids[i]));}
        auto* extra=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(extra);
        assert(!full.Enqueue(1,snapshots[0],unused) && unused==99);
        assert(!full.Enqueue(1,extra,unused) && unused==99);
        assert(full.Cancel(1,ids[0]));BdwExecRequestState status{};
        assert(full.Reap(1,ids[0],status)==snapshots[0] && status==BdwExecRequestState::Cancelled);
        assert(BdwExecSnapshotRelease(snapshots[0],false));
        assert(full.Enqueue(1,extra,unused) && unused>ids[31]);
        for(unsigned i=1;i<32;++i){assert(full.Cancel(1,ids[i]));assert(full.Reap(1,ids[i],status)==snapshots[i]);assert(BdwExecSnapshotRelease(snapshots[i],false));}
        assert(full.Cancel(1,unused) && full.Reap(1,unused,status)==extra && BdwExecSnapshotRelease(extra,false));
    }
    {
        BdwExecQueue queue;auto* first=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        auto* second=BdwExecSnapshotCreate(f,exec,2,0,8,64);auto* third=BdwExecSnapshotCreate(f,exec,2,0,8,64);
        assert(first && second && third);uint64_t a=0,b=0,c=0,active=0;BdwExecSnapshot* current=nullptr;
        assert(queue.Enqueue(7,first,a) && queue.Enqueue(8,second,b) && queue.Enqueue(7,third,c) && a<b && b<c);
        auto* queued=BdwGpuFileGetBuffer(f,replacement);assert(queued && BdwGpuBufferGpuUses(queued)==3);
        FakeUser waitUser;BdwI915UserAccess waitAccess{&waitUser,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        struct WaitQueued {uint32_t handle,flags;int64_t timeout;} wait{replacement,0,0};
        struct Busy {uint32_t handle,busy;} busy{replacement,0};
        const auto busyCmd=ioctlCommand(0x57,sizeof(busy));
        assert(!BdwIntelRenderIoctl(f,busyCmd,reinterpret_cast<uint64_t>(&busy),waitAccess) && busy.busy==0x10001);
        struct Advice {uint32_t handle,advice,retained;} advice{replacement,1,0};
        const auto adviceCmd=ioctlCommand(0x66,sizeof(advice));
        assert(!BdwIntelRenderIoctl(f,adviceCmd,reinterpret_cast<uint64_t>(&advice),waitAccess) && advice.retained==1);
        advice.advice=0;advice.retained=0;
        assert(!BdwIntelRenderIoctl(f,adviceCmd,reinterpret_cast<uint64_t>(&advice),waitAccess) && advice.retained==1);
        advice.advice=2;assert(BdwIntelRenderIoctl(f,adviceCmd,reinterpret_cast<uint64_t>(&advice),waitAccess)==-22);
        busy.handle=0;assert(BdwIntelRenderIoctl(f,busyCmd,reinterpret_cast<uint64_t>(&busy),waitAccess)==-2);busy.handle=replacement;
        assert(BdwIntelRenderIoctl(f,ioctlCommand(0x6c,sizeof(wait)),reinterpret_cast<uint64_t>(&wait),waitAccess)==-62);
        BdwExecRequestView view{};assert(!queue.Observe(8,a,view) && !queue.Cancel(8,a));
        assert(queue.Cancel(8,b) && queue.StartNext(active,current) && active==a && current==first);
        assert(!queue.StartNext(active,current) && !queue.Cancel(7,a));
        BdwExecRequestState state{};assert(!queue.Reap(7,a,state));
        assert(queue.Finish(a,true,true) && BdwGpuBufferGpuUses(queued)==1);
        assert(queue.Reap(7,a,state)==first && state==BdwExecRequestState::Complete);
        assert(BdwExecSnapshotRelease(first,false));
        assert(queue.Reap(8,b,state)==second && state==BdwExecRequestState::Cancelled && BdwExecSnapshotRelease(second,false));
        assert(queue.StartNext(active,current) && active==c && current==third && queue.Finish(c,true,true));
        assert(!BdwGpuBufferGpuUses(queued));
        assert(!BdwIntelRenderIoctl(f,busyCmd,reinterpret_cast<uint64_t>(&busy),waitAccess) && !busy.busy);
        assert(BdwIntelRenderIoctl(f,ioctlCommand(0x6c,sizeof(wait)),reinterpret_cast<uint64_t>(&wait),waitAccess)==0);
        assert(queue.Reap(7,c,state)==third && BdwExecSnapshotRelease(third,false));
        assert(!BdwGpuBufferGpuUses(queued));BdwGpuBufferUnref(queued);
    }
    {
        auto* failureFile=BdwGpuFileCreate();assert(failureFile);
        const auto activeHandle=BdwGpuFileCreateBuffer(failureFile,4096,rounded);
        const auto queuedHandle=BdwGpuFileCreateBuffer(failureFile,4096,rounded);
        BdwExecObjectInput activeObject{activeHandle,0x10000,4096,16};
        BdwExecObjectInput queuedObject{queuedHandle,0x20000,4096,16};
        auto* activeSnapshot=BdwExecSnapshotCreate(failureFile,&activeObject,1,0,0,8);
        auto* queuedSnapshot=BdwExecSnapshotCreate(failureFile,&queuedObject,1,0,0,8);
        const auto activeContextId=BdwGpuFileCreateContext(failureFile),queuedContextId=BdwGpuFileCreateContext(failureFile);
        auto* activeContext=BdwGpuFileGetContext(failureFile,activeContextId);auto* queuedContext=BdwGpuFileGetContext(failureFile,queuedContextId);
        assert(activeContext && queuedContext && BdwExecSnapshotBindContext(activeSnapshot,activeContext) && BdwExecSnapshotBindContext(queuedSnapshot,queuedContext));
        BdwExecQueue queue;uint64_t activeId=0,queuedId=0,currentId=0;BdwExecSnapshot* current=nullptr;
        assert(activeSnapshot && queuedSnapshot && queue.Enqueue(7,activeSnapshot,activeId) && queue.Enqueue(8,queuedSnapshot,queuedId));
        assert(queue.StartNext(currentId,current) && currentId==activeId && current==activeSnapshot);
        assert(queue.Finish(activeId,false,false) && queue.Poisoned());
        assert(BdwGpuContextLoss(activeContext)==1 && BdwGpuContextLoss(queuedContext)==2);
        BdwGpuContextUnref(activeContext);BdwGpuContextUnref(queuedContext);
        FakeUser waiter;BdwI915UserAccess access{&waiter,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        struct WaitFailed {uint32_t handle,flags;int64_t timeout;} wait{activeHandle,0,0};
        const auto waitCmd=ioctlCommand(0x6c,sizeof(wait));
        assert(BdwIntelRenderIoctl(failureFile,waitCmd,reinterpret_cast<uint64_t>(&wait),access)==-5);
        wait.handle=queuedHandle;assert(BdwIntelRenderIoctl(failureFile,waitCmd,reinterpret_cast<uint64_t>(&wait),access)==-5);
        BdwExecRequestView view{};BdwExecRequestState state{};
        assert(queue.Observe(8,queuedId,view) && view.state==BdwExecRequestState::Failed);
        assert(!queue.Reap(7,activeId,state) && !queue.StartNext(currentId,current));
        assert(queue.Reap(8,queuedId,state)==queuedSnapshot && BdwExecSnapshotRelease(queuedSnapshot,false));
        // Test teardown models out-of-band recovery; the ledger still
        // quarantines the active snapshot and never claims it was stopped.
        assert(BdwExecSnapshotRelease(activeSnapshot,true));BdwGpuFileUnref(failureFile);
    }
    {
        auto* failureFile=BdwGpuFileCreate();assert(failureFile);
        const auto a=BdwGpuFileCreateBuffer(failureFile,4096,rounded),b=BdwGpuFileCreateBuffer(failureFile,4096,rounded);
        BdwExecObjectInput inputs[]={{a,0x10000,4096,16},{b,0x20000,4096,16}};
        auto* first=BdwExecSnapshotCreate(failureFile,&inputs[0],1,0,0,8);
        auto* second=BdwExecSnapshotCreate(failureFile,&inputs[1],1,0,0,8);
        auto* firstBuffer=BdwGpuFileGetBuffer(failureFile,a);auto* secondBuffer=BdwGpuFileGetBuffer(failureFile,b);
        BdwSubmissionQueue service;assert(first && second && firstBuffer && secondBuffer && service.Init());
        assert(service.Submit(first)==0 && service.Submit(second)==0);
        uint64_t id=0;BdwExecSnapshot* active=nullptr;assert(service.Take(id,active) && active==first);
        assert(!service.Finish(id,false,false));
        assert(BdwGpuBufferGpuFailed(firstBuffer) && BdwGpuBufferGpuUses(firstBuffer)==1);
        assert(BdwGpuBufferGpuFailed(secondBuffer) && !BdwGpuBufferGpuUses(secondBuffer));
        auto* denied=BdwExecSnapshotCreate(failureFile,&inputs[1],1,0,0,8);assert(denied);
        assert(service.Submit(denied)==-5 && BdwExecSnapshotRelease(denied,false));
        // Out-of-band recovery is modeled only for teardown of the quarantined request.
        assert(BdwExecSnapshotRelease(first,true));BdwGpuBufferUnref(firstBuffer);BdwGpuBufferUnref(secondBuffer);BdwGpuFileUnref(failureFile);
    }
    {
        auto* failureFile=BdwGpuFileCreate();assert(failureFile);
        const auto a=BdwGpuFileCreateBuffer(failureFile,4096,rounded),b=BdwGpuFileCreateBuffer(failureFile,4096,rounded);
        BdwExecObjectInput inputs[]={{a,0x10000,4096,16},{b,0x20000,4096,16}};
        auto* first=BdwExecSnapshotCreate(failureFile,&inputs[0],1,0,0,8);
        auto* second=BdwExecSnapshotCreate(failureFile,&inputs[1],1,0,0,8);
        auto* queuedBuffer=BdwGpuFileGetBuffer(failureFile,b);FakeWorker host;host.success=host.safelyStopped=false;
        BdwSubmissionWorker worker;assert(first && second && queuedBuffer && worker.Start(host.Hooks(),0));
        assert(worker.Submit(first)==0 && worker.Submit(second)==0 && worker.Step()==BdwSubmissionStep::Recover);
        assert(BdwGpuBufferGpuFailed(queuedBuffer) && !BdwGpuBufferGpuUses(queuedBuffer));
        auto* rejected=BdwExecSnapshotCreate(failureFile,&inputs[1],1,0,0,8);assert(rejected);
        assert(worker.Submit(rejected)==-5 && BdwExecSnapshotRelease(rejected,false));
        assert(BdwExecSnapshotRelease(first,true));BdwGpuBufferUnref(queuedBuffer);BdwGpuFileUnref(failureFile);
    }
    {
        BdwExecObjectInput spaced[]={{replacement,0x20000,4096,16},{next,0x40010000,4096,16|64}};
        auto* snapshot=BdwExecSnapshotCreate(f,spaced,2,0,8,64);assert(snapshot);
        uint32_t count=0;assert(BdwExecSnapshotTableCount(snapshot,count) && count==6);
        TableAllocator failed;failed.failAt=4;BdwExecTables failedTables;
        assert(!failedTables.Prepare(failed,snapshot) && failed.freed==3);
        TableAllocator alloc,wrong;BdwExecTables tables;BdwPpgttMapping workspace[3]{};uint32_t ring[56]{};
        assert(tables.Prepare(alloc,snapshot) && tables.RootDma()==0x60000000ull);
        assert(tables.Inspect(alloc,[](const BdwOwnedPage* p,uint32_t n){return p && n==6;}));
        assert(!tables.Publish(alloc) && !tables.BeginUse(alloc));
        assert(tables.Build(alloc,snapshot,scratch,workspace,3,0x30000000,99,false,ring));
        assert(!workspace[0].writable && workspace[1].writable && workspace[2].writable);
        assert(!tables.Build(alloc,snapshot,scratch,workspace,2,0x30000000,99,false,ring));
        assert(!tables.Publish(alloc));
        assert(tables.Build(alloc,snapshot,scratch,workspace,3,0x30000000,99,false,ring));
        uint32_t status=99;assert(!tables.Publish(alloc));alloc.dmaStatus=3;
        assert(!tables.InspectDma(alloc,status) && status==3 && !tables.Publish(alloc));
        alloc.dmaStatus=0;assert(tables.InspectDma(alloc,status) && status==0);
        alloc.publishOk=false;assert(!tables.Publish(alloc));alloc.publishOk=true;
        assert(tables.Publish(alloc) && !tables.BeginUse(wrong) && tables.BeginUse(alloc));
        assert(!tables.ReleaseUnsubmitted(alloc) && !tables.ReleaseStopped(alloc,false));
        assert(tables.ReleaseStopped(alloc,true) && alloc.freed==6 && !tables.RootDma());
        assert(BdwExecSnapshotRelease(snapshot,false));
    }
    {
        BdwExecObjectInput spaced[]={{replacement,0x20000,4096,16},{next,0x40010000,4096,16|4}};
        struct ExecObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
        struct ExecBuffer {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
        const ExecObject objects[]={{next,0,0,4096,0x40010000,16|4,0,0},{replacement,0,0,4096,0x20000,16,0,0}};
        const ExecBuffer request{reinterpret_cast<uint64_t>(objects),2,8,64,0,0,0,0,1u<<11,0,0};
        FakeUser input;BdwI915UserAccess user{&input,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
        BdwExecSnapshot* snapshot=nullptr;
        assert(BdwIntelCopyExecSnapshot(f,reinterpret_cast<uint64_t>(&request),user,&snapshot)==0 && snapshot);
        RenderAllocator renderAlloc;TableAllocator tableAlloc;BdwRenderOneShot shot;BdwExecTables tables;
        BdwPpgttMapping workspace[3]{};
        assert(shot.Prepare(renderAlloc,65536,42,99,renderStaging) && tables.Prepare(tableAlloc,snapshot));
        assert(shot.AttachExternalSnapshot(renderAlloc,tables,tableAlloc,snapshot,workspace,3));
        assert(shot.InspectPrepared(renderAlloc,[&](const BdwOwnedPage* p,unsigned,const BdwRenderPlan&){
            const auto* regs=reinterpret_cast<const uint32_t*>(p[1].cpu);
            const auto* ring=reinterpret_cast<const uint32_t*>(p[20].cpu);
            assert(regs[49]==uint32_t(tables.RootDma()>>32) && regs[51]==uint32_t(tables.RootDma()));
            assert(ring[39]==0x18800101 && ring[40]==0x20008);return true;}));
        uint32_t status=99;assert(tables.InspectDma(tableAlloc,status) && status==0);
        assert(tables.Publish(tableAlloc) && shot.PublishPrepared(renderAlloc));
        assert(!shot.ArmSnapshot(renderAlloc));RenderBackend backend;
        assert(!shot.Recycle(renderAlloc) && !tables.Recycle());
        const bool flushSupported=BdwExecSnapshotSyncForDevice(snapshot);
        assert(BdwExecSnapshotInspectDma(snapshot)==BdwDmaSnapshotOk);
        if(flushSupported){
            assert(shot.ArmExternalSnapshot(renderAlloc,tableAlloc));
            assert(!tables.ReleaseUnsubmitted(tableAlloc) && !tables.ReleaseStopped(tableAlloc,false));
            assert(shot.ExecuteTakenOver(renderAlloc,backend,0x800000));
            assert(shot.PollContextSwitch(backend)==BdwCsbPoll::Complete && shot.RetireAfterReset(backend,true));
            assert(shot.ReleaseStoppedSnapshot(renderAlloc) && shot.ReleaseStopped(renderAlloc));
            assert(!shot.Recycle(renderAlloc));
            assert(tables.ReleaseStopped(tableAlloc,true) && tableAlloc.freed==6);
            assert(shot.Recycle(renderAlloc) && tables.Recycle());
            auto* second=BdwExecSnapshotCreate(f,spaced,2,0,8,64);assert(second);
            BdwExecQueue queue;uint64_t request=0,active=0;BdwExecSnapshot* activeSnapshot=nullptr;
            assert(queue.Enqueue(42,second,request) && queue.StartNext(active,activeSnapshot) && active==request && activeSnapshot==second);
            assert(shot.Prepare(renderAlloc,65536,42,100,renderStaging) && tables.Prepare(tableAlloc,second));
            assert(shot.AttachExternalSnapshot(renderAlloc,tables,tableAlloc,second,workspace,3));
            assert(tables.InspectDma(tableAlloc,status) && tables.Publish(tableAlloc) && shot.PublishPrepared(renderAlloc));
            assert(BdwExecSnapshotSyncForDevice(second) && BdwExecSnapshotInspectDma(second)==BdwDmaSnapshotOk);
            assert(shot.ArmExternalSnapshot(renderAlloc,tableAlloc));RenderBackend secondBackend;
            assert(!shot.DetachStoppedSnapshot(renderAlloc,second));
            assert(shot.ExecuteTakenOver(renderAlloc,secondBackend,0x800000));
            assert(shot.PollContextSwitch(secondBackend)==BdwCsbPoll::Complete && shot.RetireAfterReset(secondBackend,true));
            assert(!shot.DetachStoppedSnapshot(renderAlloc,nullptr));
            assert(shot.DetachStoppedSnapshot(renderAlloc,second) && shot.ReleaseStopped(renderAlloc));
            assert(tables.ReleaseStopped(tableAlloc,true) && tableAlloc.freed==12 && renderAlloc.freed==74);
            assert(queue.Finish(request,true,true));BdwExecRequestState state{};
            assert(queue.Reap(42,request,state)==second && state==BdwExecRequestState::Complete);
            assert(BdwExecSnapshotRelease(second,false));
        } else {
            assert(shot.DetachSnapshot(renderAlloc)==snapshot && shot.ReleasePrepared(renderAlloc));
            assert(tables.ReleaseUnsubmitted(tableAlloc) && BdwExecSnapshotRelease(snapshot,false));
        }
    }
    {
        auto* imageSnapshot=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(imageSnapshot);
        RenderAllocator allocator;BdwRenderOneShot shot;
        assert(shot.Prepare(allocator,65536,42,99,renderStaging));
        BdwPpgttMapping workspace[3]{};
        assert(shot.AttachSnapshot(allocator,imageSnapshot,workspace,3));
        assert(!shot.ReleasePrepared(allocator) && !shot.ArmSnapshot(allocator));
        assert(shot.InspectPrepared(allocator,[](const BdwOwnedPage* p,unsigned,const BdwRenderPlan&){
            const auto* ring=reinterpret_cast<const uint32_t*>(p[20].cpu);
            const auto* leaf=reinterpret_cast<const uint64_t*>(p[35].cpu);
            assert(ring[39]==0x18800101 && ring[40]==0x20008 && (leaf[16]&3)==3 && (leaf[32]&3)==1);return true;}));
        allocator.publishOk=false;assert(!shot.PublishPrepared(allocator) && !shot.ArmSnapshot(allocator));
        allocator.publishOk=true;assert(shot.PublishPrepared(allocator) && !shot.ArmSnapshot(allocator));
        assert(shot.DetachSnapshot(allocator)==imageSnapshot);
        assert(!shot.AttachSnapshot(allocator,imageSnapshot,workspace,3));
        assert(shot.ReleasePrepared(allocator) && allocator.freed==37);
        assert(BdwExecSnapshotRelease(imageSnapshot,false));
    }
    {
        auto* snapshot=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(snapshot);
        auto* batch=BdwGpuFileGetBuffer(f,replacement);auto* data=BdwGpuFileGetBuffer(f,next);
        RenderAllocator allocator;RenderBackend backend;BdwRenderOneShot shot;BdwPpgttMapping workspace[3]{};
        assert(batch && data && shot.Prepare(allocator,65536,42,99,renderStaging));
        assert(BdwExecSnapshotReserve(snapshot) && !BdwExecSnapshotReserve(snapshot));
        assert(BdwGpuBufferGpuUses(batch)==1 && BdwGpuBufferGpuUses(data)==1);
        assert(shot.AttachSnapshot(allocator,snapshot,workspace,3) && shot.PublishPrepared(allocator));
        const bool flushSupported=BdwExecSnapshotSyncForDevice(snapshot);
        assert(BdwExecSnapshotInspectDma(snapshot)==BdwDmaSnapshotOk);
        if(flushSupported){
            assert(shot.ArmSnapshot(allocator) && BdwGpuBufferGpuUses(batch)==1 && BdwGpuBufferGpuUses(data)==1);
            assert(shot.ExecuteTakenOver(allocator,backend,0x800000));
            assert(shot.PollContextSwitch(backend)==BdwCsbPoll::Complete);
            assert(!shot.ReleaseStoppedSnapshot(allocator) && !shot.ReleaseStopped(allocator));
            assert(shot.RetireAfterReset(backend,true));
            for(auto pte:backend.ptes)assert(!pte);
            assert(shot.ReleaseStoppedSnapshot(allocator));
            assert(!BdwGpuBufferGpuUses(batch) && !BdwGpuBufferGpuUses(data));
            assert(shot.ReleaseStopped(allocator) && allocator.freed==37);
        } else {
            assert(shot.DetachSnapshot(allocator)==snapshot && shot.ReleasePrepared(allocator));
            assert(BdwExecSnapshotRelease(snapshot,false));
        }
        BdwGpuBufferUnref(batch);BdwGpuBufferUnref(data);
    }
    {
        auto* retired=BdwExecSnapshotCreate(f,exec,2,0,8,64);assert(retired);
        BdwPpgttMapping work[3]{};uint32_t ring[56]{};
        assert(BdwExecSnapshotBuildRenderImage(retired,scratch,tables,4,work,3,0x30000000,99,false,ring));
        const bool flushed=BdwExecSnapshotSyncForDevice(retired);
        assert(BdwExecSnapshotInspectDma(retired)==BdwDmaSnapshotOk);
        if(flushed){
            auto* watched=BdwGpuFileGetBuffer(f,replacement);assert(watched && BdwExecSnapshotArm(retired,true));
            assert(BdwGpuBufferGpuUses(watched)==1 && !BdwExecSnapshotRetire(retired,false));
            assert(BdwExecSnapshotRetire(retired,true) && !BdwGpuBufferGpuUses(watched));
            assert(!BdwExecSnapshotRetire(retired,true));BdwGpuBufferUnref(watched);
        }
        assert(BdwExecSnapshotRelease(retired,false));
    }
    auto* submission=BdwExecSnapshotCreate(f,exec,2,0,8,0);assert(submission);
    auto* watchedBatch=BdwGpuFileGetBuffer(f,replacement);auto* watchedData=BdwGpuFileGetBuffer(f,next);
    assert(watchedBatch && watchedData && !BdwGpuBufferGpuUses(watchedBatch) && !BdwGpuBufferGpuUses(watchedData));
    assert(BdwExecSnapshotPageCount(submission)==3 && BdwExecSnapshotBatchAddress(submission)==0x20008 && BdwExecSnapshotBatchLength(submission)==4088);
    uint32_t neededTables=0;assert(BdwExecSnapshotTableCount(submission,neededTables) && neededTables==4);
    assert(!BdwExecSnapshotArm(submission,true));
    vtdReads=0;assert(BdwExecSnapshotInspectDma(submission)==BdwDmaSnapshotOk && vtdReads==2);
    translated=true;assert(BdwExecSnapshotInspectDma(submission)==BdwDmaFirstVtdRestricted);translated=false;
    wrongOwner=true;assert(BdwExecSnapshotInspectDma(submission)==BdwDmaPageOwner);wrongOwner=false;
    vtdReads=0;changingVtd=true;assert(BdwExecSnapshotInspectDma(submission)==BdwDmaVtdChanged);changingVtd=false;
    resetArmed=false;assert(BdwExecSnapshotInspectDma(submission)==BdwDmaResetUnarmed);resetArmed=true;
    BdwPpgttMapping execMaps[3]{};uint32_t execRing[56]{};
    assert(!BdwExecSnapshotBuildRenderImage(submission,scratch,tables,4,execMaps,3,0,99,false,execRing));
    auto& aliasedRing=*reinterpret_cast<uint32_t (*)[56]>(pageTables[7]);
    assert(!BdwExecSnapshotBuildRenderImage(submission,scratch,tables,4,execMaps,3,0x30000000,99,false,aliasedRing));
    assert(BdwExecSnapshotBuildRenderImage(submission,scratch,tables,4,execMaps,3,0x30000000,99,false,execRing));
    assert(execRing[39]==0x18800101 && execRing[40]==0x20008 && execRing[41]==0 &&
           execRing[50]==0x30000000 && execRing[52]==99);
    assert(execMaps[0].address==0x10000 && execMaps[0].writable && execMaps[2].address==0x20000 && !execMaps[2].writable);
    const auto* leaf=reinterpret_cast<const uint64_t*>(pageTables[7]);
    assert(leaf[16]==(execMaps[0].dma|0x83) && leaf[32]==(execMaps[2].dma|0x81));
    uint32_t a,ebx,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(ebx),"=c"(c),"=d"(d):"a"(1),"c"(0));
    const uint32_t line=((ebx>>8)&255)*8;
    const bool flushSupported=(d&(1u<<19)) && line>=16 && line<=4096 && !(line&(line-1)) && !(4096%line);
    assert(BdwExecSnapshotSyncForDevice(submission)==flushSupported && !BdwExecSnapshotSyncForCpu(submission,true));
    assert(!BdwExecSnapshotArm(submission,true)); // failed later DMA observation cleared its proof
    assert(BdwExecSnapshotInspectDma(submission)==BdwDmaSnapshotOk);
    if(flushSupported){
        assert(!BdwExecSnapshotBuildRenderImage(submission,scratch,tables,4,execMaps,3,0,99,false,execRing));
        assert(!BdwExecSnapshotArm(submission,true)); // failed rebuild revokes the image proof
        assert(BdwExecSnapshotBuildRenderImage(submission,scratch,tables,4,execMaps,3,0x30000000,99,false,execRing));
        assert(!BdwExecSnapshotArm(submission,false));
        assert(BdwExecSnapshotArm(submission,true) && !BdwExecSnapshotArm(submission,true) && !BdwExecSnapshotRelease(submission,false));
        assert(BdwGpuBufferGpuUses(watchedBatch)==1 && BdwGpuBufferGpuUses(watchedData)==1);
        assert(BdwGpuBufferBeginGpuUse(watchedData) && BdwGpuBufferGpuUses(watchedData)==2);
        assert(BdwExecSnapshotMarkFailed(submission) && BdwGpuBufferGpuFailed(watchedBatch) && BdwGpuBufferGpuFailed(watchedData));
        assert(!BdwGpuBufferBeginGpuUse(watchedBatch));
        assert(BdwExecSnapshotInspectDma(submission)==BdwDmaNotPrepared);
        assert(!BdwExecSnapshotSyncForDevice(submission) && !BdwExecSnapshotSyncForCpu(submission,false));
        assert(BdwExecSnapshotSyncForCpu(submission,true));
        assert(!BdwExecSnapshotBuildPpgtt(submission,scratch,tables,4,execMaps,3));
    } else assert(!BdwExecSnapshotArm(submission,true));
    exec[1].address=0x20000;assert(!BdwExecSnapshotCreate(f,exec,2,0,0,0));exec[1].address=0x10000;
    exec[0].flags|=4;assert(!BdwExecSnapshotCreate(f,exec,2,0,0,0));exec[0].flags&=~uint64_t{4};
    exec[0].address=uint64_t{1}<<48;assert(!BdwExecSnapshotCreate(f,exec,2,0,0,0));exec[0].address=0x20000;
    assert(!BdwExecSnapshotCreate(f,exec,2,0,4096,0));
    exec[1].handle=replacement;assert(!BdwExecSnapshotCreate(f,exec,2,0,0,0));exec[1].handle=next;
    uint32_t handles[]={replacement,next};BdwGpuBuffer* resolved[2]{};
    assert(BdwGpuFileResolveBuffers(f,handles,2,resolved) && resolved[0] && resolved[1]);
    assert(BdwGpuFileCloseBuffer(f,replacement) && BdwGpuFileCloseBuffer(f,next));
    assert(BdwGpuBufferBytes(resolved[0])==4096 && BdwGpuBufferBytes(resolved[1])==8192);
    assert(BdwExecSnapshotRelease(submission,true));
    assert(!BdwGpuBufferGpuUses(watchedBatch));
    if(flushSupported){assert(BdwGpuBufferGpuUses(watchedData)==1 && BdwGpuBufferEndGpuUse(watchedData));}
    assert(!BdwGpuBufferGpuUses(watchedData));
    assert(!BdwGpuBufferEndGpuUse(watchedBatch));
    BdwGpuBufferUnref(watchedBatch);BdwGpuBufferUnref(watchedData);
    BdwGpuBufferUnref(resolved[0]);BdwGpuBufferUnref(resolved[1]);
    handles[0]=BdwGpuFileCreateBuffer(f,4096,rounded);handles[1]=handles[0];
    resolved[0]=resolved[1]=nullptr;assert(!BdwGpuFileResolveBuffers(f,handles,2,resolved));
    assert(!resolved[0] && !resolved[1] && !BdwGpuFileResolveBuffers(other,handles,1,resolved));
    BdwGpuFileUnref(f);BdwGpuFileUnref(other);assert(BdwGpuBufferBytes(mapped)==8192);BdwGpuBufferUnref(mapped);
    allocated=0;f=BdwGpuFileCreate();assert(f);FakeUser fake{};BdwI915UserAccess user{&fake,ReadUser,WritableUser,WriteUser,UserNow,UserSleep};
    {
        auto* manyFile=BdwGpuFileCreate();assert(manyFile);uint32_t many[brook::BdwMaxSubmissionFences]{};
        for(auto& id:many){id=BdwGpuFileCreateSync(manyFile,false);assert(id);}
        struct TimelineArray {uint64_t handles,points;uint32_t count,flags;};
        TimelineArray signal{reinterpret_cast<uint64_t>(many),0,brook::BdwMaxSubmissionFences,0};
        const auto cmd=ioctlCommand(0xcd,sizeof(signal)),arg=reinterpret_cast<uint64_t>(&signal);
        assert(!BdwIntelRenderIoctl(manyFile,cmd,arg,user));signal.count++;
        assert(BdwIntelRenderIoctl(manyFile,cmd,arg,user)==-22);
        for(auto id:many)assert(BdwGpuFileDestroySync(manyFile,id));BdwGpuFileUnref(manyFile);
    }
    struct Version {int32_t major,minor,patch,pad;uint64_t nameLen,name,dateLen,date,descLen,desc;};
    char driver[8]{};Version version{};version.nameLen=sizeof(driver);version.name=reinterpret_cast<uint64_t>(driver);
    assert(BdwIntelRenderIoctl(f,ioctlCommand(0,sizeof(version)),reinterpret_cast<uint64_t>(&version),user)==0);
    assert(version.major==1 && version.minor==6 && version.nameLen==4 && !std::memcmp(driver,"i915",4));
    fake.writeOk=false;assert(BdwIntelRenderIoctl(f,ioctlCommand(0,sizeof(version)),reinterpret_cast<uint64_t>(&version),user)==-14);fake.writeOk=true;
    struct Capability {uint64_t id,value;} capability{5,99};
    const auto capCmd=ioctlCommand(0x0c,sizeof(capability)),capArg=reinterpret_cast<uint64_t>(&capability);
    assert(BdwIntelRenderIoctl(f,capCmd,capArg,user)==0 && capability.value==0);
    capability.id=0x13;assert(BdwIntelRenderIoctl(f,capCmd,capArg,user)==0 && capability.value==0);capability.id=5;
    struct GetParam {int32_t param,pad;uint64_t value;};
    int32_t parameter=-1;GetParam get{4,0,reinterpret_cast<uint64_t>(&parameter)};
    const auto getCmd=ioctlCommand(0x46,sizeof(get)),getArg=reinterpret_cast<uint64_t>(&get);
    assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==0x1616);
    get.param=32;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==9);
    get.param=9;parameter=-1;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==0);
    get.param=37;parameter=-1;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==0);
    get.param=55;parameter=-1;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==0);
    user.renderExperiment=true;get.param=9;
    assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==0); // no owner, no capability
    user.submit=UserSubmit;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==1);
    get.param=55;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==1);
    get.param=45;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==0);
    assert(BdwIntelRenderIoctl(f,capCmd,capArg,user)==0 && capability.value==3);
    capability.id=0x13;assert(BdwIntelRenderIoctl(f,capCmd,capArg,user)==0 && capability.value==1);
    capability.id=0x14;assert(BdwIntelRenderIoctl(f,capCmd,capArg,user)==0 && capability.value==0);
    capability.id=0x13;auto* savedSleep=user.sleepNs;user.sleepNs=nullptr;
    assert(BdwIntelRenderIoctl(f,capCmd,capArg,user)==0 && capability.value==0);user.sleepNs=savedSleep;
    get.param=37;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==1);
    get.param=40;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==4);
    get.param=49;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==1);
    get.param=50;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==1);
    user.submit=nullptr;user.renderExperiment=false;
    assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==0);
    get.param=19;parameter=-1;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==0 && parameter==1);
    get.param=999;parameter=-1;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==-22 && parameter==-1);
    get.param=4;fake.readable=false;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==-14);fake.readable=true;
    fake.writeOk=false;assert(BdwIntelRenderIoctl(f,getCmd,getArg,user)==-14);fake.writeOk=true;
    struct Create {uint64_t size;uint32_t handle,pad;} create{4097,0,0};
    const auto createCmd=ioctlCommand(0x5b,sizeof(create)),createArg=reinterpret_cast<uint64_t>(&create);
    fake.readable=false;assert(BdwIntelRenderIoctl(f,createCmd,createArg,user)==-14);fake.readable=true;
    fake.writable=false;assert(BdwIntelRenderIoctl(f,createCmd,createArg,user)==-14);fake.writable=true;
    const unsigned oldFrees=frees;fake.writeOk=false;assert(BdwIntelRenderIoctl(f,createCmd,createArg,user)==-14 && frees==oldFrees+1);fake.writeOk=true;
    create={4097,0,0};assert(BdwIntelRenderIoctl(f,createCmd,createArg,user)==0 && create.handle==1 && create.size==8192);
    struct Wait {uint32_t handle,flags;int64_t timeout;} wait{create.handle,0,5000000};
    const auto waitCmd=ioctlCommand(0x6c,sizeof(wait)),waitArg=reinterpret_cast<uint64_t>(&wait);
    assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==0 && wait.timeout==5000000);
    auto* waited=BdwGpuFileGetBuffer(f,create.handle);assert(waited && BdwGpuBufferBeginGpuUse(waited));
    wait.timeout=0;assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==-62);
    wait.timeout=2000000;fake.now=0;fake.sleeps=0;
    assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==-62 && !wait.timeout && fake.sleeps==2);
    wait.timeout=5000000;fake.now=0;fake.sleeps=0;fake.completeAt=2;fake.waiting=waited;
    assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==0 && wait.timeout==3000000);
    assert(BdwGpuBufferBeginGpuUse(waited));wait.timeout=-1;fake.completeAt=0;fake.interrupt=true;
    assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==-4 && wait.timeout==-1);
    fake.interrupt=false;fake.waiting=nullptr;assert(BdwGpuBufferEndGpuUse(waited));
    assert(BdwGpuBufferBeginCpuTransition(waited) && !BdwGpuBufferBeginCpuTransition(waited));
    assert(BdwGpuBufferTryCpuTransition(waited)==BdwCpuTransitionResult::Busy);
    assert(!BdwGpuBufferBeginGpuUse(waited) && BdwGpuBufferEndCpuTransition(waited));
    assert(BdwGpuBufferTryCpuTransition(nullptr)==BdwCpuTransitionResult::Failed);
    assert(BdwGpuBufferBeginGpuUse(waited));
    assert(BdwGpuBufferTryCpuTransition(waited)==BdwCpuTransitionResult::Busy && BdwGpuBufferGpuUses(waited)==1);
    assert(BdwGpuBufferEndGpuUse(waited));
    assert(BdwGpuBufferTryCpuTransition(waited)==(flushSupported?BdwCpuTransitionResult::Ready:BdwCpuTransitionResult::Failed));
    struct SetDomain {uint32_t handle,read,write;} domain{create.handle,1,0};
    const auto domainCmd=ioctlCommand(0x5f,sizeof(domain));
    assert(BdwGpuBufferBeginGpuUse(waited));fake.sleeps=0;fake.completeAt=1;fake.waiting=waited;
    assert(BdwIntelRenderIoctl(f,domainCmd,reinterpret_cast<uint64_t>(&domain),user)==(flushSupported?0:-5));
    assert(fake.sleeps==1 && !BdwGpuBufferGpuUses(waited));fake.waiting=nullptr;
    assert(BdwGpuBufferBeginGpuUse(waited));fake.interrupt=true;
    assert(BdwIntelRenderIoctl(f,domainCmd,reinterpret_cast<uint64_t>(&domain),user)==-4);
    fake.interrupt=false;assert(BdwGpuBufferEndGpuUse(waited));
    domain.read=2;assert(BdwIntelRenderIoctl(f,domainCmd,reinterpret_cast<uint64_t>(&domain),user)==-22);domain.read=1;
    wait.flags=1;assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==-22);wait.flags=0;
    wait.handle=999;assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==-2);wait.handle=create.handle;
    assert(BdwGpuBufferBeginGpuUse(waited));BdwGpuBufferMarkGpuFailed(waited);
    assert(BdwGpuBufferTryCpuTransition(waited)==BdwCpuTransitionResult::Failed);
    uint32_t sampledUses=0;bool sampledFailed=false;
    BdwGpuBufferGpuStatus(waited,sampledUses,sampledFailed);assert(sampledUses==1 && sampledFailed);
    struct FailedBusy {uint32_t handle,busy;} failedBusy{create.handle,0};
    assert(BdwIntelRenderIoctl(f,ioctlCommand(0x57,sizeof(failedBusy)),reinterpret_cast<uint64_t>(&failedBusy),user)==-5);
    assert(!BdwGpuBufferBeginGpuUse(waited));
    assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==-5 && BdwGpuBufferGpuUses(waited)==1);
    assert(BdwGpuBufferEndGpuUse(waited));
    BdwGpuBufferGpuStatus(waited,sampledUses,sampledFailed);assert(!sampledUses && sampledFailed);
    BdwGpuBufferUnref(waited);
    struct ExecObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
    struct ExecBuffer {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
    ExecObject eo{create.handle,0,0,4096,0x20000,16,0,0};
    ExecBuffer eb{reinterpret_cast<uint64_t>(&eo),1,8,64,0,0,0,0,1u<<11,0,0};
    BdwExecSnapshot* decoded=nullptr;auto execArg=reinterpret_cast<uint64_t>(&eb);
    assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==-25);
    assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==0 && decoded);
    assert(BdwExecSnapshotBatchAddress(decoded)==0x20008 && BdwExecSnapshotBatchLength(decoded)==64);
    assert(BdwExecSnapshotRelease(decoded,false));decoded=nullptr;
    user.renderExperiment=true;
    assert(!BdwIntelCopyExecSnapshot(f,execArg,user,&decoded) && BdwExecSnapshotContext(decoded));
    assert(BdwExecSnapshotRelease(decoded,false));decoded=nullptr;user.renderExperiment=false;
    eb.context=1;assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-2 && !decoded);
    const uint32_t contextId=BdwGpuFileCreateContext(f);assert(contextId);
    {
        struct ResetStats {uint32_t id,flags,resets,active,pending,pad;} stats{contextId,0,99,99,99,0};
        const auto command=ioctlCommand(0x72,sizeof(stats)),arg=reinterpret_cast<uint64_t>(&stats);
        assert(!BdwIntelRenderIoctl(f,command,arg,user) && !stats.resets && !stats.active && !stats.pending);
        const auto lostId=BdwGpuFileCreateContext(f);auto* lost=BdwGpuFileGetContext(f,lostId);assert(lost);
        BdwGpuContextMarkLost(lost,false);BdwGpuContextMarkLost(lost,true);BdwGpuContextUnref(lost);
        stats.id=lostId;assert(!BdwIntelRenderIoctl(f,command,arg,user) && stats.active==1 && stats.pending==1);
        assert(BdwGpuFileDestroyContext(f,lostId));
    }
    user.submitterPid=1234;user.submitterIncarnation=987654;eb.context=contextId;assert(!BdwIntelCopyExecSnapshot(f,execArg,user,&decoded) && decoded);
    uint32_t recordedPid=0,recordedContext=0;BdwExecSnapshotOrigin(decoded,recordedPid,recordedContext);
    assert(recordedPid==1234 && recordedContext==contextId && BdwExecSnapshotOwner(decoded)==987654);
    BdwExecSnapshot* reusedPid=nullptr;user.submitterIncarnation=987655;
    assert(!BdwIntelCopyExecSnapshot(f,execArg,user,&reusedPid) && reusedPid && BdwExecSnapshotOwner(reusedPid)==987655);
    assert(BdwExecSnapshotOwner(decoded)==987654 && BdwExecSnapshotRelease(reusedPid,false));user.submitterPid=0;user.submitterIncarnation=0;
    auto* retainedContext=BdwExecSnapshotContext(decoded);assert(retainedContext && BdwGpuFileDestroyContext(f,contextId));
    static uint32_t snapshotContext[20*1024];bool contextPresent=true;
    assert(BdwGpuContextBegin(retainedContext,snapshotContext,contextPresent) && !contextPresent);
    assert(BdwGpuContextFinish(retainedContext,nullptr,true) && BdwExecSnapshotRelease(decoded,false));decoded=nullptr;
    eb.context=0x100000000ull;assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-22 && !decoded);eb.context=0;
    {
        const auto secondHandle=BdwGpuFileCreateBuffer(f,4096,rounded);assert(secondHandle);
        ExecObject pair[]={{create.handle,0,0,4096,0x20000,16|64|128,0,0},{secondHandle,0,0,4096,0x40000,16|4,0,0}};
        const auto originalObjects=eb.objects;eb.objects=reinterpret_cast<uint64_t>(pair);eb.count=2;
        eb.flags=(1u<<11)|(1u<<12)|(1u<<18);
        assert(!BdwIntelCopyExecSnapshot(f,execArg,user,&decoded) && decoded && BdwExecSnapshotBatchAddress(decoded)==0x20008);
        assert(BdwExecSnapshotRelease(decoded,false));decoded=nullptr;
        eb.flags=(1u<<12)|(1u<<18);assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-22 && !decoded);
        eb.objects=originalObjects;eb.count=1;eb.flags=1u<<11;
        assert(BdwGpuFileCloseBuffer(f,secondHandle));
    }
    eb.flags|=1u<<17;assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-22 && !decoded);eb.flags=1u<<11;
    eb.start=1;assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-22 && !decoded);eb.start=8;
    eo.relocCount=1;assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-22 && !decoded);eo.relocCount=0;
    eo.reserved1=1;assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-22 && !decoded);eo.reserved1=0;
    eo.flags=0;assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-22 && !decoded);eo.flags=16;
    eb.objects=~uint64_t{0}-1;assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-14 && !decoded);
    eb.objects=reinterpret_cast<uint64_t>(&eo);fake.readable=false;
    assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-14 && !decoded);fake.readable=true;
    fake.failRead=fake.reads+2;
    assert(BdwIntelCopyExecSnapshot(f,execArg,user,&decoded)==-14 && !decoded);fake.failRead=0;
    {
        const auto fresh=BdwGpuFileCreateBuffer(f,4096,rounded);assert(fresh);
        eo.handle=fresh;BdwExecQueue queue;fake.queue=&queue;user.submit=UserSubmit;
        {
            const auto inputId=BdwGpuFileCreateSync(f,true),outputId=BdwGpuFileCreateSync(f,false);assert(inputId && outputId);
            struct Fence {uint32_t handle,flags;} entries[]={{inputId,1},{outputId,2}};
            eb.flags=(1u<<11)|(1u<<19);eb.clipCount=2;eb.clips=reinterpret_cast<uint64_t>(entries);
            auto* output=BdwGpuFileGetSync(f,outputId);assert(output && !BdwGpuSyncAcquire(output));
            fake.rejectSubmission=true;assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==-16);
            assert(!BdwGpuSyncAcquire(output));fake.rejectSubmission=false;
            assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user));
            auto* fence=BdwGpuSyncAcquire(output);assert(fence && !BdwFenceState(fence));
            struct Wait {uint64_t handles;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
            Wait pending{reinterpret_cast<uint64_t>(&outputId),0,1,1,99,0,0};
            const auto waitCmd=ioctlCommand(0xc3,sizeof(pending)),waitArg=reinterpret_cast<uint64_t>(&pending);
            assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==-62);
            uint64_t active=0;BdwExecSnapshot* item=nullptr;BdwExecRequestState state{};
            assert(queue.StartNext(active,item) && active==fake.request && queue.Finish(active,true,true));
            assert(!BdwIntelRenderIoctl(f,waitCmd,waitArg,user));
            assert(BdwFenceState(fence)==1 && queue.Reap(9,active,state)==item && BdwExecSnapshotRelease(item,false));
            BdwFenceUnref(fence);BdwGpuSyncUnref(output);
            assert(BdwGpuFileDestroySync(f,inputId) && BdwGpuFileDestroySync(f,outputId));
            eb.flags=1u<<11;eb.clipCount=0;eb.clips=0;fake.request=0;
        }
        {
            const auto syncId=BdwGpuFileCreateSync(f,false);assert(syncId);
            auto* sync=BdwGpuFileGetSync(f,syncId);assert(sync);
            struct Fence {uint32_t handle,flags;} entry{syncId,2};uint64_t point=5;
            struct Extension {uint64_t next;uint32_t name,flags,reserved[4];uint64_t count,handles,points;};
            Extension extension{0,0,0,{0,0,0,0},1,reinterpret_cast<uint64_t>(&entry),reinterpret_cast<uint64_t>(&point)};
            eb.flags=(1u<<11)|(1u<<21);eb.clipCount=0;eb.clips=reinterpret_cast<uint64_t>(&extension);
            fake.rejectSubmission=true;assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==-16);
            assert(!BdwGpuSyncFindTimeline(sync,5));fake.rejectSubmission=false;
            assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user));
            auto* fence=BdwGpuSyncFindTimeline(sync,5);assert(fence && !BdwFenceState(fence));
            const auto destId=BdwGpuFileCreateSync(f,false);auto* dest=BdwGpuFileGetSync(f,destId);assert(dest);
            struct Transfer {uint32_t source,destination;uint64_t sourcePoint,destinationPoint;uint32_t flags,pad;};
            Transfer transfer{syncId,destId,5,0,0,0};const auto transferCmd=ioctlCommand(0xcc,sizeof(transfer)),transferArg=reinterpret_cast<uint64_t>(&transfer);
            assert(!BdwIntelRenderIoctl(f,transferCmd,transferArg,user));auto* copied=BdwGpuSyncAcquire(dest);
            assert(copied==fence && !BdwFenceState(copied));BdwFenceUnref(copied);
            transfer.destinationPoint=1;assert(!BdwIntelRenderIoctl(f,transferCmd,transferArg,user));transfer.destinationPoint=0;
            transfer.flags=1;assert(BdwIntelRenderIoctl(f,transferCmd,transferArg,user)==-22);transfer.flags=0;
            transfer.sourcePoint=6;assert(BdwIntelRenderIoctl(f,transferCmd,transferArg,user)==-22);
            transfer.source=destId;transfer.sourcePoint=0;transfer.destination=destId;assert(!BdwIntelRenderIoctl(f,transferCmd,transferArg,user));
            const auto timelineDestId=BdwGpuFileCreateSync(f,false);auto* timelineDest=BdwGpuFileGetSync(f,timelineDestId);assert(timelineDest);
            transfer.source=syncId;transfer.sourcePoint=5;transfer.destination=timelineDestId;transfer.destinationPoint=10;
            assert(!BdwIntelRenderIoctl(f,transferCmd,transferArg,user));auto* joined=BdwGpuSyncFindTimeline(timelineDest,10);
            assert(joined && !BdwFenceState(joined) && BdwGpuSyncQueryTimeline(timelineDest,false)==0);
            transfer.destinationPoint=0;
            const auto waitId=BdwGpuFileCreateSync(f,false);auto* waitSync=BdwGpuFileGetSync(f,waitId);assert(waitSync);
            transfer.source=waitId;transfer.destination=destId;transfer.sourcePoint=0;transfer.flags=2;
            fake.now=0;fake.sleeps=0;fake.completeAt=2;fake.transferSync=waitSync;fake.transferFence=fence;
            assert(!BdwIntelRenderIoctl(f,transferCmd,transferArg,user) && fake.sleeps==2);
            fake.transferSync=nullptr;assert(BdwGpuSyncReplace(waitSync,nullptr));fake.interrupt=true;
            assert(BdwIntelRenderIoctl(f,transferCmd,transferArg,user)==-4);fake.interrupt=false;fake.now=0;
            assert(BdwIntelRenderIoctl(f,transferCmd,transferArg,user)==-62);
            BdwGpuSyncUnref(waitSync);assert(BdwGpuFileDestroySync(f,waitId));
            BdwGpuSyncUnref(dest);assert(BdwGpuFileDestroySync(f,destId));
            struct TimelineWait {uint64_t handles,points;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
            TimelineWait tw{reinterpret_cast<uint64_t>(&syncId),reinterpret_cast<uint64_t>(&point),0,1,0,99,0,0};
            const auto twCmd=ioctlCommand(0xca,sizeof(tw)),twArg=reinterpret_cast<uint64_t>(&tw);
            TimelineWait emptyTimeline{0,0,0,0,4,77,0,0};
            assert(!BdwIntelRenderIoctl(f,twCmd,reinterpret_cast<uint64_t>(&emptyTimeline),user) && emptyTimeline.first==77);
            emptyTimeline.flags=16;assert(BdwIntelRenderIoctl(f,twCmd,reinterpret_cast<uint64_t>(&emptyTimeline),user)==-22);
            assert(BdwIntelRenderIoctl(f,twCmd,twArg,user)==-62);
            tw.flags=1;assert(BdwIntelRenderIoctl(f,twCmd,twArg,user)==-62);
            tw.flags=4;assert(!BdwIntelRenderIoctl(f,twCmd,twArg,user));
            tw.flags=8;tw.deadline=123;assert(BdwIntelRenderIoctl(f,twCmd,twArg,user)==-62);
            tw.flags=0;tw.deadline=0;
            struct TimelineArray {uint64_t handles,points;uint32_t count,flags;};uint64_t queryPoint=99;
            TimelineArray query{reinterpret_cast<uint64_t>(&syncId),reinterpret_cast<uint64_t>(&queryPoint),1,0};
            const auto queryCmd=ioctlCommand(0xcb,sizeof(query)),queryArg=reinterpret_cast<uint64_t>(&query);
            assert(!BdwIntelRenderIoctl(f,queryCmd,queryArg,user) && queryPoint==0);
            query.flags=1;assert(!BdwIntelRenderIoctl(f,queryCmd,queryArg,user) && queryPoint==5);query.flags=0;
            const auto zeroId=BdwGpuFileCreateSync(f,false);auto* zeroSync=BdwGpuFileGetSync(f,zeroId);assert(zeroSync);
            assert(BdwIntelSyncReplaceAdmitted(zeroSync,fence));
            TimelineArray zeroSignal{reinterpret_cast<uint64_t>(&zeroId),0,1,0};
            assert(!BdwIntelRenderIoctl(f,ioctlCommand(0xcd,sizeof(zeroSignal)),reinterpret_cast<uint64_t>(&zeroSignal),user));
            auto* zeroFence=BdwGpuSyncAcquire(zeroSync);assert(zeroFence && !BdwFenceState(zeroFence));
            uint64_t active=0;BdwExecSnapshot* item=nullptr;BdwExecRequestState state{};
            assert(queue.StartNext(active,item) && active==fake.request && queue.Finish(active,true,true));
            assert(BdwFenceState(fence)==1 && queue.Reap(9,active,state)==item && BdwExecSnapshotRelease(item,false));
            assert(BdwFenceState(zeroFence)==1);BdwFenceUnref(zeroFence);BdwGpuSyncUnref(zeroSync);assert(BdwGpuFileDestroySync(f,zeroId));
            assert(BdwFenceState(joined)==1 && BdwGpuSyncQueryTimeline(timelineDest,false)==10);
            BdwFenceUnref(joined);BdwGpuSyncUnref(timelineDest);assert(BdwGpuFileDestroySync(f,timelineDestId));
            assert(!BdwIntelRenderIoctl(f,twCmd,twArg,user) && tw.first==0);
            assert(!BdwIntelRenderIoctl(f,queryCmd,queryArg,user) && queryPoint==5);
            point=6;assert(BdwIntelRenderIoctl(f,twCmd,twArg,user)==-22);
            tw.flags=4;assert(BdwIntelRenderIoctl(f,twCmd,twArg,user)==-62);tw.flags=0;
            TimelineArray signal{reinterpret_cast<uint64_t>(&syncId),reinterpret_cast<uint64_t>(&point),1,0};
            assert(!BdwIntelRenderIoctl(f,ioctlCommand(0xcd,sizeof(signal)),reinterpret_cast<uint64_t>(&signal),user));
            assert(!BdwIntelRenderIoctl(f,twCmd,twArg,user));
            assert(!BdwIntelRenderIoctl(f,queryCmd,queryArg,user) && queryPoint==6);point=5;
            BdwFenceUnref(fence);
            extension.reserved[0]=1;assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==-22);
            BdwGpuSyncUnref(sync);assert(BdwGpuFileDestroySync(f,syncId));
            eb.flags=1u<<11;eb.clipCount=0;eb.clips=0;fake.request=0;
        }
        {
            // A real submission with more than the previous 16-fence limit:
            // all timeline outputs must be published and retire together.
            constexpr uint32_t count=32;struct Fence {uint32_t handle,flags;} entries[count]{};
            uint64_t points[count]{};
            for(uint32_t i=0;i<count;++i){entries[i]={BdwGpuFileCreateSync(f,false),2};assert(entries[i].handle);points[i]=i+1;}
            struct Extension {uint64_t next;uint32_t name,flags,reserved[4];uint64_t count,handles,points;};
            Extension extension{0,0,0,{0,0,0,0},count,reinterpret_cast<uint64_t>(entries),reinterpret_cast<uint64_t>(points)};
            eb.flags=(1u<<11)|(1u<<21);eb.clips=reinterpret_cast<uint64_t>(&extension);eb.clipCount=0;
            assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user));
            uint64_t active=0;BdwExecSnapshot* item=nullptr;BdwExecRequestState state{};
            assert(queue.StartNext(active,item) && active==fake.request && queue.Finish(active,true,true));
            assert(queue.Reap(9,active,state)==item && BdwExecSnapshotRelease(item,false));
            uint32_t handles[count];for(uint32_t i=0;i<count;++i)handles[i]=entries[i].handle;
            struct TimelineWait {uint64_t handles,points;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
            TimelineWait waitAll{reinterpret_cast<uint64_t>(handles),0,0,count,1,99,0,0};
            assert(!BdwIntelRenderIoctl(f,ioctlCommand(0xca,sizeof(waitAll)),reinterpret_cast<uint64_t>(&waitAll),user));
            for(uint32_t i=0;i<count;++i){auto* object=BdwGpuFileGetSync(f,entries[i].handle);assert(object);
                auto* fence=BdwGpuSyncFindTimeline(object,points[i]);assert(fence && BdwFenceState(fence)==1);
                BdwFenceUnref(fence);BdwGpuSyncUnref(object);assert(BdwGpuFileDestroySync(f,entries[i].handle));}
            eb.flags=1u<<11;eb.clips=0;fake.request=0;
        }
        {
            // Distinct handles can alias one sync object; repeated wait/signal
            // entries must publish in order and remain atomic on rejection.
            const auto id=BdwGpuFileCreateSync(f,true);auto* sync=BdwGpuFileGetSync(f,id);assert(sync);
            const auto alias=BdwGpuFileImportSync(f,sync);assert(alias && alias!=id);
            struct Fence {uint32_t handle,flags;} entries[]={{id,1},{alias,1},{alias,2},{id,2}};
            uint64_t points[]={0,0,2,3};
            struct Extension {uint64_t next;uint32_t name,flags,reserved[4];uint64_t count,handles,points;};
            Extension extension{0,0,0,{0,0,0,0},4,reinterpret_cast<uint64_t>(entries),reinterpret_cast<uint64_t>(points)};
            eb.flags=(1u<<11)|(1u<<21);eb.clips=reinterpret_cast<uint64_t>(&extension);eb.clipCount=0;
            fake.rejectSubmission=true;assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==-16);
            assert(!BdwGpuSyncFindTimeline(sync,2));fake.rejectSubmission=false;
            assert(!BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user));
            auto* two=BdwGpuSyncFindTimeline(sync,2);auto* three=BdwGpuSyncFindTimeline(sync,3);
            assert(two && three && two!=three && !BdwFenceState(two) && !BdwFenceState(three));
            uint64_t active=0;BdwExecSnapshot* item=nullptr;BdwExecRequestState state{};
            assert(queue.StartNext(active,item) && active==fake.request && queue.Finish(active,true,true));
            assert(BdwFenceState(two)==1 && BdwFenceState(three)==1 && queue.Reap(9,active,state)==item && BdwExecSnapshotRelease(item,false));
            BdwFenceUnref(two);BdwFenceUnref(three);BdwGpuSyncUnref(sync);
            assert(BdwGpuFileDestroySync(f,id) && BdwGpuFileDestroySync(f,alias));
            eb.flags=1u<<11;eb.clips=0;fake.request=0;
        }
        {
            const auto id=BdwGpuFileCreateSync(f,false);auto* sync=BdwGpuFileGetSync(f,id);assert(sync);
            const auto alias=BdwGpuFileImportSync(f,sync);assert(alias);uint32_t handles[]={id,alias,id};uint64_t points[]={2,4,3};
            struct TimelineArray {uint64_t handles,points;uint32_t count,flags;};
            TimelineArray signal{reinterpret_cast<uint64_t>(handles),reinterpret_cast<uint64_t>(points),3,0};
            assert(!BdwIntelRenderIoctl(f,ioctlCommand(0xcd,sizeof(signal)),reinterpret_cast<uint64_t>(&signal),user));
            auto* point=BdwGpuSyncFindTimeline(sync,4);assert(point && BdwFenceState(point)==1 && BdwGpuSyncQueryTimeline(sync,true)==4);
            BdwFenceUnref(point);BdwGpuSyncUnref(sync);assert(BdwGpuFileDestroySync(f,id) && BdwGpuFileDestroySync(f,alias));
        }
        eb.context=contextId;assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==-2 && !fake.request);eb.context=0;
        fake.rejectSubmission=true;
        assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==-16);
        auto* buffer=BdwGpuFileGetBuffer(f,fresh);assert(buffer && !BdwGpuBufferGpuUses(buffer));
        fake.rejectSubmission=false;
        assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==0 && fake.request);
        assert(BdwGpuBufferGpuUses(buffer)==1);
        wait.handle=fresh;wait.timeout=0;assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==-62);
        assert(queue.Cancel(9,fake.request));BdwExecRequestState state{};
        assert(!BdwGpuBufferGpuUses(buffer) && BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==0);
        auto* cancelled=queue.Reap(9,fake.request,state);assert(cancelled && state==BdwExecRequestState::Cancelled);
        assert(BdwExecSnapshotRelease(cancelled,false));
        if(flushSupported){
            assert(BdwIntelRenderIoctl(f,ioctlCommand(0x69,sizeof(eb)),execArg,user)==0);
            uint64_t active=0;BdwExecSnapshot* job=nullptr;
            assert(queue.StartNext(active,job) && active==fake.request && job);
            RenderAllocator renderAlloc;TableAllocator tableAlloc;BdwRenderOneShot shot;BdwExecTables tables;
            BdwPpgttMapping workspace[1]{};RenderBackend backend;uint32_t dma=99;
            assert(shot.Prepare(renderAlloc,65536,42,99,renderStaging) && tables.Prepare(tableAlloc,job));
            assert(shot.AttachExternalSnapshot(renderAlloc,tables,tableAlloc,job,workspace,1));
            assert(tables.InspectDma(tableAlloc,dma) && tables.Publish(tableAlloc) && shot.PublishPrepared(renderAlloc));
            assert(BdwExecSnapshotSyncForDevice(job) && BdwExecSnapshotInspectDma(job)==BdwDmaSnapshotOk);
            assert(shot.ArmExternalSnapshot(renderAlloc,tableAlloc) && shot.ExecuteTakenOver(renderAlloc,backend,0x800000));
            assert(shot.PollContextSwitch(backend)==BdwCsbPoll::Complete && shot.RetireAfterReset(backend,true));
            assert(shot.DetachStoppedSnapshot(renderAlloc,job) && shot.ReleaseStopped(renderAlloc) && tables.ReleaseStopped(tableAlloc,true));
            assert(queue.Finish(active,true,true) && !BdwGpuBufferGpuUses(buffer));
            assert(BdwIntelRenderIoctl(f,waitCmd,waitArg,user)==0);
            assert(queue.Reap(9,active,state)==job && state==BdwExecRequestState::Complete && BdwExecSnapshotRelease(job,false));
        }
        BdwGpuBufferUnref(buffer);
        assert(BdwGpuFileCloseBuffer(f,fresh));user.submit=nullptr;fake.queue=nullptr;
    }
    struct Offset {uint32_t handle,pad;uint64_t offset,flags,extensions;} map{create.handle,0,0,2,0};
    const auto mapCmd=ioctlCommand(0x64,sizeof(map));assert(BdwIntelRenderIoctl(f,mapCmd,reinterpret_cast<uint64_t>(&map),user)==0 && map.offset);
    auto* pinMapping=BdwGpuFileGetMapping(f,map.offset+4096,4096,offset);assert(pinMapping && offset==4096);
    map.flags=1;assert(BdwIntelRenderIoctl(f,mapCmd,reinterpret_cast<uint64_t>(&map),user)==-22);
    struct Close {uint32_t handle,pad;} close{create.handle,0};
    assert(BdwIntelRenderIoctl(f,ioctlCommand(9,sizeof(close)),reinterpret_cast<uint64_t>(&close),user)==0);
    assert(BdwIntelRenderIoctl(f,ioctlCommand(9,sizeof(close)),reinterpret_cast<uint64_t>(&close),user)==-2);
    assert(BdwGpuBufferBytes(pinMapping)==8192);BdwGpuFileUnref(f);BdwGpuBufferUnref(pinMapping);
    assert(BdwIntelRenderIoctl(nullptr,createCmd,createArg,user)==-9);
}

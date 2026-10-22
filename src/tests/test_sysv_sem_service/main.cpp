// Executes the actual service, with only heap/clock/process/scheduler boundaries
// replaced. Real threads exercise atomic transactions and wake/registration races.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <functional>
#include <thread>
#include <vector>
#define BROOK_HOST_TEST 1
#include "../../kernel/src/sysv_sem.h"
#include "../../kernel/src/spinlock.h"
namespace brook {
constexpr unsigned MAX_PROCESSES=1024;
struct SupplementaryGroups {unsigned count;unsigned ids[2];};
struct Process {
    unsigned pid,tgid,euid=1000,egid=1000;
    uint64_t incarnation=1,wakeupTick=0;
    bool terminalPending=false;
    int pendingWakeup=0;
    std::atomic<int> refs{1},regions{0};
    std::mutex mutex;
    std::condition_variable cv;
    explicit Process(unsigned id):pid(id),tgid(id){}
};
static std::atomic<int> allocations{0};
static bool failAllocation=false;
extern "C" void* kmalloc(uint64_t size) {if(failAllocation)return nullptr;auto* p=std::malloc(size);if(p)++allocations;return p;}
extern "C" void kfree(void* p) {if(p){--allocations;std::free(p);}}
static SupplementaryGroups* ProcessGroupsAcquire(Process*) {return nullptr;}
static void ProcessGroupsRelease(SupplementaryGroups*) {}
static void ProcessRef(Process* p){++p->refs;}
static void ProcessUnref(Process* p){--p->refs;}
static void SchedulerKernelRegionEnter(Process* p){++p->regions;}
static void SchedulerKernelRegionLeave(Process* p){--p->regions;}
static bool SemInterrupted(Process* p){return __atomic_load_n(&p->terminalPending,__ATOMIC_ACQUIRE);}
static void SchedulerUnblock(Process* p){std::lock_guard<std::mutex> hold(p->mutex);__atomic_store_n(&p->pendingWakeup,1,__ATOMIC_RELEASE);p->cv.notify_all();}
static uint64_t SemNowNs(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
static uint64_t SemWakeupTick(uint64_t deadline){return deadline;}
static void SchedulerBlockInterruptible(Process* p){
    std::unique_lock<std::mutex> hold(p->mutex);
    auto predicate=[&]{return __atomic_load_n(&p->pendingWakeup,__ATOMIC_ACQUIRE)||SemInterrupted(p);};
    if(p->wakeupTick==~uint64_t{0}) {if(!p->cv.wait_for(hold,std::chrono::seconds(3),predicate))std::abort();}
    else p->cv.wait_until(hold,std::chrono::steady_clock::time_point(std::chrono::nanoseconds(p->wakeupTick)),predicate);
}
static uint64_t RtcNow(){return 123456;}
#define BROOK_SYSV_SEM_RETRY_TEST 1
static std::function<void()> retryHook;
static void SemRetryTestHook(){if(retryHook)retryHook();}
#include "../../kernel/src/sysv_sem_service.inc"
}
using namespace brook;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
static int ctl(Process& p,int id,int cmd,int num=0,int value=0){return SysvSemControl(&p,id,num,cmd,value,nullptr,nullptr,0);}
static int op(Process& p,int id,int delta,unsigned flags=0,unsigned num=0){SysvSemOp s{uint16_t(num),int16_t(delta),uint16_t(flags)};return SysvSemOperate(&p,id,&s,1);}
static void awaitWaiter(Process& p,int id,int command=14){
    for(unsigned i=0;i<1000;++i){if(ctl(p,id,command)==1)return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    CHECK(false);
}
int main(){
    Process a(1),b(2),c(3);b.euid=2000;b.egid=2000;
    int id=SysvSemGet(&a,123,2,01000|0600);CHECK(id>0);
    CHECK(SysvSemGet(&a,123,2,01000|02000|0600)==-17);
    CHECK(SysvSemGet(&a,123,3,0600)==-22);
    CHECK(SysvSemGet(&b,123,0,0400)==-13);CHECK(ctl(b,id,12)==-13);CHECK(ctl(b,id,0)==-1);
    SysvSemDs ds{};CHECK(SysvSemControl(&a,id,0,2,0,&ds,nullptr,0)==0);
    CHECK(ds.count==2 && ds.permission.uid==1000 && ds.permission.cuid==1000 && ds.changeTime==123456);
    CHECK(ctl(a,id,16,0,1)==0);SysvSemOp batch[]={{0,-1,0},{1,-1,SysvSemNowait}};
    CHECK(SysvSemOperate(&a,id,batch,2)==-11 && ctl(a,id,12)==1);
    // SETALL is all-or-nothing on invalid input.
    uint16_t invalid[]={2,32768};CHECK(SysvSemControl(&a,id,0,17,0,nullptr,invalid,2)==-34);CHECK(ctl(a,id,12)==1);
    failAllocation=true;CHECK(op(a,id,-1)==-12);failAllocation=false;CHECK(ctl(a,id,12)==1);
    // Blocking negative and zero operations, including a wake before wait.
    CHECK(ctl(a,id,16,0,0)==0);std::atomic<int> result{-999};
    std::thread blocked([&]{result=op(c,id,-1);});awaitWaiter(a,id);CHECK(op(a,id,1)==0);blocked.join();CHECK(result==0 && ctl(a,id,14)==0);
    CHECK(ctl(a,id,16,0,1)==0);
    std::thread zero([&]{result=op(c,id,0);});awaitWaiter(a,id,15);CHECK(op(a,id,-1)==0);zero.join();CHECK(result==0);
    // A finite wait expires without changing values/counts; a ready zero-time op succeeds.
    SysvSemOp timed{0,-1,0};
    CHECK(SysvSemOperate(&c,id,&timed,1,SemNowNs()+10000000)==-11);
    CHECK(ctl(a,id,14)==0 && ctl(a,id,12)==0);
    timed.delta=1;CHECK(SysvSemOperate(&c,id,&timed,1,SemNowNs())==0);
    CHECK(ctl(a,id,16,0,0)==0);
    // Interrupted operation unlinks exactly once; removal has EIDRM precedence.
    std::thread interrupted([&]{result=op(c,id,-1);});awaitWaiter(a,id);
    __atomic_store_n(&c.terminalPending,true,__ATOMIC_RELEASE);SchedulerUnblock(&c);interrupted.join();CHECK(result==-4 && ctl(a,id,14)==0);
    c.terminalPending=false;
    std::thread removed([&]{result=op(c,id,-1);});awaitWaiter(a,id);CHECK(ctl(a,id,0)==0);removed.join();CHECK(result==-43);CHECK(ctl(a,id,12)==-22);
    // Removal in the unlocked retry gap still belongs to an already-waiting call.
    id=SysvSemGet(&a,0,1,0600);
    retryHook=[&]{CHECK(ctl(a,id,0)==0);};
    std::thread retryRemoved([&]{result=op(c,id,-1);});awaitWaiter(a,id);
    SchedulerUnblock(&c);retryRemoved.join();retryHook={};CHECK(result==-43);
    // IPC_SET uses IPC owner authority rather than filesystem chown rules.
    id=SysvSemGet(&a,0,1,0600);
    CHECK(SysvSemControl(&a,id,0,2,0,&ds,nullptr,0)==0);
    ds.permission.uid=2000;ds.permission.gid=3000;
    CHECK(SysvSemControl(&a,id,0,1,0,&ds,nullptr,0)==0);
    CHECK(SysvSemControl(&a,id,0,2,0,&ds,nullptr,0)==0 && ds.permission.cuid==1000);
    ds.permission.uid=~uint32_t{0};CHECK(SysvSemControl(&a,id,0,1,0,&ds,nullptr,0)==-22);
    CHECK(ctl(a,id,0)==0);
    // Ordinary fork has no alias. Shared CLONE_SYSVSEM state survives first exit.
    id=SysvSemGet(&a,0,1,0600);CHECK(ctl(a,id,16,0,5)==0);
    CHECK(op(a,id,-2,SysvSemUndo)==0);CHECK(SysvSemClone(&a,&c,false)==0);SysvSemExit(&c);CHECK(ctl(a,id,12)==3);
    CHECK(SysvSemClone(&a,&c,true)==0);SysvSemExit(&a);CHECK(ctl(c,id,12)==3);
    SysvSemExit(&c);CHECK(ctl(a,id,12)==5);SysvSemExit(&c);CHECK(ctl(a,id,12)==5);
    // Share before either task uses SEM_UNDO; thread identity is not inferred.
    CHECK(SysvSemClone(&a,&c,true)==0);CHECK(op(c,id,-1,SysvSemUndo)==0);SysvSemExit(&c);CHECK(ctl(a,id,12)==4);SysvSemExit(&a);CHECK(ctl(a,id,12)==5);
    // SETVAL clears every group's adjustments, not merely the setter's.
    CHECK(op(a,id,-1,SysvSemUndo)==0);CHECK(op(c,id,-1,SysvSemUndo)==0);
    CHECK(ctl(a,id,16,0,7)==0);SysvSemExit(&a);SysvSemExit(&c);CHECK(ctl(a,id,12)==7);
    // Positive operation's negative undo clamps to zero, never blocks exit.
    CHECK(op(a,id,2,SysvSemUndo)==0);CHECK(op(c,id,-8)==0);SysvSemExit(&a);CHECK(ctl(c,id,12)==0);
    // Removed IDs cannot affect replacements, even with pending undo records.
    CHECK(op(a,id,1,SysvSemUndo)==0);CHECK(ctl(a,id,0)==0);
    int replacement=SysvSemGet(&a,0,1,0600);CHECK(replacement>id);SysvSemExit(&a);CHECK(ctl(a,replacement,12)==0);
    CHECK(ctl(a,replacement,0)==0);
    // Concurrent transactions on one set retain exact increments.
    id=SysvSemGet(&a,0,1,0600);
    std::vector<std::thread> workers;
    for(unsigned j=0;j<4;++j)workers.emplace_back([&,j]{Process p(10+j);for(unsigned i=0;i<500;++i)CHECK(op(p,id,1)==0);SysvSemExit(&p);CHECK(p.refs==1 && p.regions==0);});
    for(auto& t:workers)t.join();CHECK(ctl(a,id,12)==2000);CHECK(ctl(a,id,0)==0);
    SysvSemExit(&a);SysvSemExit(&b);SysvSemExit(&c);SemDrainGarbage();
    CHECK(allocations==0 && a.refs==1 && b.refs==1 && c.refs==1);
    CHECK(a.regions==0 && b.regions==0 && c.regions==0 && !semWaiters && !semSets);
    std::puts("SysV semaphore service: PASS");
}

// Lock-free kernel-region entry against termination (kernel_region.h).
// 1. Direct cases for every branch.
// 2. Exhaustive interleaving model with x86-TSO store buffers: an owner
//    (enter, leave, operation boundary, enter) against a terminator holding
//    the ready lock. Checks that no state is reachable where the owner is
//    inside a region while the process is Terminated, that entry fails only
//    when Terminated, and (self-check) that removing either fence breaks it.
// 3. Threaded stress with the real header on the host CPU.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "../../kernel/src/kernel_region.h"
using namespace brook;
static unsigned failures=0;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"kernel region line %d: %s\n",__LINE__,#x);++failures;}}while(0)

struct Proc { uint32_t kernelRegionDepth=0; bool kernelOperationProtected=false; uint8_t terminalClaim=0; uint8_t state=0; bool terminalPending=false; };
static bool Term(Proc* p){return __atomic_load_n(&p->state,__ATOMIC_ACQUIRE)==1;}

static void DirectCases(){
    Proc p;KernelRegionUndo u;
    CHECK(KernelRegionOwnerEnter(&p,Term,&u)==KernelRegionEnter::Entered && p.kernelRegionDepth==1 && p.kernelOperationProtected);
    CHECK(KernelRegionOwnerEnter(&p,Term,&u)==KernelRegionEnter::Entered && p.kernelRegionDepth==2 && u.depth==1 && u.latch);
    CHECK(KernelRegionBusyForTerminationLocked(&p) && !p.terminalClaim);
    CHECK(KernelRegionOwnerLeave(&p) && KernelRegionOwnerLeave(&p) && !KernelRegionOwnerLeave(&p) && p.kernelRegionDepth==0);
    CHECK(KernelRegionBusyForTerminationLocked(&p)); // latch alone defers
    p.kernelOperationProtected=false;
    CHECK(!KernelRegionBusyForTerminationLocked(&p) && p.terminalClaim==1); // claim kept for publication
    // Claim seen but terminator deferred / not yet published: resolve succeeds.
    CHECK(KernelRegionOwnerEnter(&p,Term,&u)==KernelRegionEnter::Resolve && KernelRegionResolveLocked(&p,Term,u) && p.kernelRegionDepth==1);
    CHECK(KernelRegionOwnerLeave(&p));p.kernelOperationProtected=false;
    // Published Terminated: resolve withdraws depth and the latch it set.
    p.state=1;
    CHECK(KernelRegionOwnerEnter(&p,Term,&u)==KernelRegionEnter::Resolve && !KernelRegionResolveLocked(&p,Term,u));
    CHECK(p.kernelRegionDepth==0 && !p.kernelOperationProtected);
    // Terminated with no claim (e.g. birth failure) still resolves to failure.
    Proc q;q.state=1;CHECK(KernelRegionOwnerEnter(&q,Term,&u)==KernelRegionEnter::Resolve && !KernelRegionResolveLocked(&q,Term,u));
    // Withdrawal keeps a latch that was already set.
    Proc r;r.kernelOperationProtected=true;r.state=1;
    CHECK(KernelRegionOwnerEnter(&r,Term,&u)==KernelRegionEnter::Resolve && !KernelRegionResolveLocked(&r,Term,u) && r.kernelOperationProtected);
}

// ---- TSO model -------------------------------------------------------------
// Shared variables: 0 depth, 1 latch, 2 claim, 3 state, 4 pending, 5 lock.
// Each thread has a FIFO store buffer; loads forward from it; fences and the
// lock's acquire/release (locked instructions) drain it. Depth updates in the
// header are locked RMWs: modelled as drain + atomic update.
struct Model {
    uint8_t mem[6]{};
    struct Store{uint8_t var,val;};
    Store buf[2][4]{};uint8_t bufN[2]{};
    uint8_t pc[2]{};       // program counters
    uint8_t reg[2][4]{};   // thread registers
    uint8_t inside=0;      // owner currently inside a successful region
    uint8_t entries=0;     // successful owner entries
    bool operator<(const Model& o)const{return std::memcmp(this,&o,sizeof(Model))<0;}
    uint8_t Load(int t,int v)const{for(int i=bufN[t]-1;i>=0;--i)if(buf[t][i].var==v)return buf[t][i].val;return mem[v];}
    bool StoreB(int t,int v,uint8_t x){if(bufN[t]==4)return false;buf[t][bufN[t]++]={uint8_t(v),x};return true;}
    void Drain(int t){for(int i=0;i<bufN[t];++i)mem[buf[t][i].var]=buf[t][i].val;bufN[t]=0;}
    void Flush1(int t){mem[buf[t][0].var]=buf[t][0].val;for(int i=1;i<bufN[t];++i)buf[t][i-1]=buf[t][i];--bufN[t];}
};
static bool g_ownerFence=true,g_termFence=true;
static unsigned g_violations=0,g_spurious=0,g_states=0,g_terminalOutcomes=0;
// Owner program: Enter; Leave; Boundary; Enter. Terminator: one locked decision.
// Returns false when the step cannot run now (lock busy); sets done.
static bool StepOwner(Model& m){
    auto& pc=m.pc[0];auto* r=m.reg[0];
    const bool second=pc>=20;const uint8_t base=second?20:0;
    switch(pc-base){
    case 0: r[0]=m.Load(0,1);
            if(g_ownerFence){m.Drain(0);m.mem[0]=uint8_t(m.mem[0]+1);}                 // depth RMW (locked: drains)
            else if(!m.StoreB(0,0,uint8_t(m.Load(0,0)+1)))return false;                 // broken variant: plain store
            pc++;return true;
    case 1: if(!m.StoreB(0,1,1))return false;pc++;return true;                         // latch=1
    case 2: if(g_ownerFence)m.Drain(0);pc++;return true;                               // fence
    case 3: r[1]=m.Load(0,2);r[2]=m.Load(0,3);pc+=(r[1]||r[2]==1)?1:3;if(!(r[1]||r[2]==1)){m.inside=1;++m.entries;}return true;
    case 4: if(m.mem[5])return false;m.Drain(0);m.mem[5]=1;                              // lock (resolve)
            if(m.Load(0,3)==1){m.mem[0]=uint8_t(m.mem[0]-1);if(!r[0])m.mem[1]=0;r[3]=1;}else{m.inside=1;++m.entries;}
            m.Drain(0);m.mem[5]=0;pc+=2;return true;
    case 6: // entered or failed
            if(r[3]){pc=40;return true;}                                                // failed: stop
            if(second){pc=40;return true;}
            pc=10;return true;
    }
    switch(pc){
    case 10: m.Drain(0);m.mem[0]=uint8_t(m.mem[0]-1);m.inside=0;pc++;return true;     // leave (CAS)
    case 11: if(m.mem[5])return false;m.Drain(0);m.mem[5]=1;                             // boundary (locked)
             if(!m.Load(0,0)){m.mem[1]=0;if(m.mem[4]){m.mem[4]=0;m.mem[3]=1;r[3]=2;}}
             m.Drain(0);m.mem[5]=0;pc=r[3]==2?40:20;return true;                        // owner self-terminates on pending
    }
    return false;
}
static bool StepTerm(Model& m){
    auto& pc=m.pc[1];
    switch(pc){
    case 0: if(m.mem[5])return false;m.Drain(1);m.mem[5]=1;pc++;return true;             // lock
    case 1: if(m.Load(1,3)==1){pc=4;return true;}if(!m.StoreB(1,2,1))return false;pc++;return true; // claim=1
    case 2: if(g_termFence)m.Drain(1);pc++;return true;
    case 3: {const bool busy=m.Load(1,0)||m.Load(1,1);
             if(busy){m.StoreB(1,2,0);m.StoreB(1,4,1);}else{m.StoreB(1,3,1);++g_terminalOutcomes;}
             pc++;return true;}
    case 4: m.Drain(1);m.mem[5]=0;pc++;return true;                                       // unlock
    case 5: pc=40;return true;
    }
    return false;
}
static void Explore(const Model& start){
    std::set<Model> seen;std::vector<Model> stack{start};
    while(!stack.empty()){
        Model m=stack.back();stack.pop_back();
        if(!seen.insert(m).second)continue;
        ++g_states;
        // Invariant: inside a region implies not Terminated in memory.
        if(m.inside && m.mem[3]==1)++g_violations;
        bool any=false;
        for(int t=0;t<2;++t){
            if(m.pc[t]!=40){Model n=m;if(t?StepTerm(n):StepOwner(n)){stack.push_back(n);any=true;}}
            if(m.bufN[t]){Model n=m;n.Flush1(t);stack.push_back(n);any=true;}
        }
        if(!any && m.pc[0]==40 && m.reg[0][3]==1 && m.mem[3]!=1)++g_spurious; // failed entry without termination
    }
}
static void ModelCheck(){
    Model start{};
    g_ownerFence=g_termFence=true;g_violations=g_spurious=g_states=g_terminalOutcomes=0;
    Explore(start);
    std::printf("TSO model: %u states, violations %u, spurious failures %u, terminal outcomes %u\n",g_states,g_violations,g_spurious,g_terminalOutcomes);
    CHECK(g_states>100 && !g_violations && !g_spurious && g_terminalOutcomes);
    // Self-check: the terminator fence, and the owner's ordering (locked RMW
    // plus fence; replaced by a plain store without fence), must be caught.
    g_ownerFence=false;g_violations=0;Explore(start);CHECK(g_violations);
    std::printf("  owner plain store, no fence: %u violating states (expected > 0)\n",g_violations);
    g_ownerFence=true;g_termFence=false;g_violations=0;Explore(start);CHECK(g_violations);
    std::printf("  without terminator fence: %u violating states (expected > 0)\n",g_violations);
    g_termFence=true;
}

// ---- Threaded stress ---------------------------------------------------------
static void Stress(){
    std::mutex ready;std::atomic<unsigned> phase{0},done{0};
    Proc p;std::atomic<bool> stop{false};
    unsigned entered=0,failedTerminated=0,deferred=0,terminated=0,violations=0;
    constexpr unsigned Rounds=200000;
    std::thread terminator([&]{
        for(unsigned i=0;i<Rounds;++i){
            while(phase.load(std::memory_order_acquire)!=2*i+1)std::this_thread::yield();
            {std::lock_guard<std::mutex> g(ready);
             if(!Term(&p)){if(KernelRegionBusyForTerminationLocked(&p))p.terminalPending=true;else{__atomic_store_n(&p.state,uint8_t(1),__ATOMIC_RELEASE);++terminated;}}}
            done.fetch_add(1,std::memory_order_acq_rel);
        }
    });
    for(unsigned i=0;i<Rounds;++i){
        p=Proc{};if(i&1)p.kernelRegionDepth=0;
        phase.store(2*i+1,std::memory_order_release); // release terminator
        for(unsigned s=0;s<(i*7)%64;++s)__asm__ volatile("pause"); // vary the race window
        KernelRegionUndo u;bool in=KernelRegionOwnerEnter(&p,Term,&u)==KernelRegionEnter::Entered;
        if(!in){std::lock_guard<std::mutex> g(ready);in=KernelRegionResolveLocked(&p,Term,u);}
        if(in){++entered;for(int k=0;k<8;++k)if(Term(&p))++violations;KernelRegionOwnerLeave(&p);if(Term(&p))++violations;}
        else {if(!Term(&p))++violations;++failedTerminated;}
        while(done.load(std::memory_order_acquire)!=i+1)std::this_thread::yield();
        if(in){if(Term(&p))++violations;if(p.terminalPending)++deferred;}
        // Operation boundary (locked): clear the latch; pending is honoured.
        {std::lock_guard<std::mutex> g(ready);if(!p.kernelRegionDepth)p.kernelOperationProtected=false;}
    }
    terminator.join();(void)stop;
    std::printf("stress: %u rounds, entered %u (deferred %u), entry failed on terminated %u, terminated %u, violations %u\n",
        Rounds,entered,deferred,failedTerminated,terminated,violations);
    CHECK(!violations && entered && failedTerminated && deferred && entered+failedTerminated==Rounds);
}

int main(){
    DirectCases();ModelCheck();Stress();
    std::printf("KERNEL_REGION %s\n",failures?"FAIL":"PASS");return failures?1:0;
}

// Actual runtime wrappers, also compiled by hosted admission tests.
namespace brook {
static FrameTimingModel<> g_frameTiming;
static SpinLock g_frameTimingLock;
static bool g_frameTimingEnabled;
// Admission hint only; the model still validates ownership under the lock.
static uintptr_t g_frameTimingOwner;
static uint64_t g_frameTimingContention;
struct FrameTimingGuard {
    bool locked=true;
    explicit FrameTimingGuard(bool record=false){
        if(!record){SpinLockAcquire(&g_frameTimingLock);return;}
        locked=SpinLockTryAcquire(&g_frameTimingLock);
        if(!locked){
            auto n=__atomic_load_n(&g_frameTimingContention,__ATOMIC_RELAXED);
            while(n!=~uint64_t(0) && !__atomic_compare_exchange_n(&g_frameTimingContention,&n,n+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
        }
    }
    ~FrameTimingGuard(){if(locked)SpinLockRelease(&g_frameTimingLock);}
};
bool FrameTimingControl(int mode,bool reset) {
    FrameTimingGuard guard;
    const bool ok=g_frameTiming.Control(mode,reset);
    if(reset || mode==0)__atomic_store_n(&g_frameTimingOwner,uintptr_t(0),__ATOMIC_RELEASE);
    __atomic_store_n(&g_frameTimingEnabled,g_frameTiming.Status().enabled,__ATOMIC_RELEASE);
    return ok;
}
FrameTimingStatus FrameTimingReadStatus(){
    FrameTimingGuard guard;auto s=g_frameTiming.Status();
    s.contendedRecords=__atomic_load_n(&g_frameTimingContention,__ATOMIC_RELAXED);return s;
}
bool FrameTimingReadRow(uint64_t epoch,unsigned index,FrameTimingRow* output) {
    FrameTimingGuard guard;return g_frameTiming.Read(epoch,index,output);
}
static uintptr_t Owner(){return reinterpret_cast<uintptr_t>(ProcessCurrent());}
static bool Clock(uint64_t* now) {
    if(KvmClockReadStableNs(now))return true;
    g_frameTiming.ClockFailure();return false;
}
void FrameTimingBegin() {
    if(!__atomic_load_n(&g_frameTimingEnabled,__ATOMIC_ACQUIRE))return;
    const auto owner=Owner();
    FrameTimingGuard guard(true);if(!guard.locked)return;uint64_t now=0;
    if(g_frameTiming.Status().enabled && Clock(&now))g_frameTiming.Begin(owner,now);
    __atomic_store_n(&g_frameTimingOwner,g_frameTiming.Active(owner)?owner:uintptr_t(0),__ATOMIC_RELEASE);
}
void FrameTimingEnd(uint64_t sleepDeadlineTicks) {
    if(!__atomic_load_n(&g_frameTimingEnabled,__ATOMIC_ACQUIRE))return;
    const auto owner=Owner();
    if(!owner || owner!=__atomic_load_n(&g_frameTimingOwner,__ATOMIC_ACQUIRE))return;
    FrameTimingGuard guard(true);if(!guard.locked)return;uint64_t now=0;
    if(g_frameTiming.Active(owner) && Clock(&now))g_frameTiming.End(owner,now,sleepDeadlineTicks);
    __atomic_store_n(&g_frameTimingOwner,uintptr_t(0),__ATOMIC_RELEASE);
}
extern "C" void FrameTimingEnter(unsigned phase,FrameScopeToken* token) {
    if(!token)return;
    *token={};
    if(!__atomic_load_n(&g_frameTimingEnabled,__ATOMIC_ACQUIRE))return;
    const auto owner=Owner();
    if(!owner || owner!=__atomic_load_n(&g_frameTimingOwner,__ATOMIC_ACQUIRE))return;
    FrameTimingGuard guard(true);if(!guard.locked)return;uint64_t now=0;
    if(!g_frameTiming.Active(owner) || !Clock(&now))return;
    if(phase>=FramePhaseCount){g_frameTiming.Invalidate();return;}
    *token=g_frameTiming.Enter(owner,static_cast<FramePhase>(phase),now);
}
extern "C" void FrameTimingLeave(const FrameScopeToken* token) {
    if(!token || !token->id || !__atomic_load_n(&g_frameTimingEnabled,__ATOMIC_ACQUIRE))return;
    const auto owner=Owner();
    if(!owner || owner!=__atomic_load_n(&g_frameTimingOwner,__ATOMIC_ACQUIRE))return;
    FrameTimingGuard guard(true);if(!guard.locked)return;uint64_t now=0;
    if(g_frameTiming.Active(owner) && Clock(&now))g_frameTiming.Leave(owner,*token,now);
}
extern "C" void FrameTimingCommand(uint32_t bytes,bool fenced,uint64_t fence) {
    if(!__atomic_load_n(&g_frameTimingEnabled,__ATOMIC_ACQUIRE))return;
    const auto owner=Owner();
    if(!owner || owner!=__atomic_load_n(&g_frameTimingOwner,__ATOMIC_ACQUIRE))return;
    FrameTimingGuard guard(true);if(!guard.locked)return;
    g_frameTiming.Command(owner,bytes,fenced,fence);
}
} // namespace brook

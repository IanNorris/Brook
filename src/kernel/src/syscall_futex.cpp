// syscall_futex.cpp -- futex, PI futexes and robust lists.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "futex_timeout.h"
#include "futex_waitv.h"
#include "panic.h"
#include "pid_namespace.h"
#include "profiler.h"
#include "sync/futex_waiter_pool.h"
#include "sync/pi_wait_graph.h"
#include "signal_restart.h"
#include "string.h"
#include "memory/file_object.h"
#include "memory/user_mapping.h"
#include "tsc_clock.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_memfd.h"
#include "syscall_memory.h"
#include "syscall_time.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_set/get_robust_list — per-thread x86-64 robust-list registration.
// ---------------------------------------------------------------------------
int64_t sys_set_robust_list(uint64_t head,uint64_t length,uint64_t,uint64_t,uint64_t,uint64_t)
{
    if(length!=24)return -EINVAL;
    auto* proc=ProcessCurrent();if(!proc || proc->isKernelThread)return -ESRCH;
    __atomic_store_n(&proc->robustListHead,head,__ATOMIC_RELEASE);
    return 0; // Like Linux, registration does not dereference user memory.
}
int64_t sys_get_robust_list(uint64_t pid,uint64_t headOut,uint64_t lengthOut,uint64_t,uint64_t,uint64_t)
{
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    const int32_t tid = static_cast<int32_t>(pid);
    if (tid < 0) return -ESRCH;
    auto* target=tid?SchedulerFindVisibleProcessReference(ProcessPidView(proc),tid):proc;
    if(!target)return -ESRCH;
    const bool held=tid!=0;
    // Conservative bounded access policy: only the calling thread group.
    // Cross-process ptrace credential/dumpability policy is not implemented.
    if (ProcessVisibleTgid(target, ProcessPidView(proc)) != ProcessVisibleTgid(proc, ProcessPidView(proc))) {if(held)SchedulerReleaseWaitReference(target);return -EPERM;}
    const uint64_t head=__atomic_load_n(&target->robustListHead,__ATOMIC_ACQUIRE),length=24;
    if(held)SchedulerReleaseWaitReference(target);
    if(!UserBufferWritable(headOut,8) || !UserBufferWritable(lengthOut,8))return -EFAULT;
    return CopyToUser(lengthOut,&length,8) && CopyToUser(headOut,&head,8)?0:-EFAULT;
}

// ---------------------------------------------------------------------------
// sys_futex (202) — fast userspace mutex with real wait queues
// ---------------------------------------------------------------------------

static constexpr int FUTEX_WAIT         = 0;
static constexpr int FUTEX_WAKE         = 1;
static constexpr int FUTEX_WAKE_OP      = 5;
static constexpr int FUTEX_LOCK_PI      = 6;
static constexpr int FUTEX_UNLOCK_PI    = 7;
static constexpr int FUTEX_TRYLOCK_PI   = 8;
static constexpr int FUTEX_WAIT_BITSET  = 9;
static constexpr int FUTEX_WAKE_BITSET  = 10;
static constexpr int FUTEX_WAIT_REQUEUE_PI = 11;
static constexpr int FUTEX_CMP_REQUEUE_PI = 12;
static constexpr int FUTEX_LOCK_PI2     = 13;
static constexpr int FUTEX_PRIVATE_FLAG    = 128;
static constexpr int FUTEX_CLOCK_REALTIME  = 256;

// Futex wait queues use private TGID/VA keys or tagged shared-object/offset
// keys. Waiters retain the chosen value key through wake, timeout and cancellation.
static constexpr uint32_t FUTEX_HASH_SIZE = 64;

// FutexWaiter and the slot pool live in sync/futex_waiter_pool.h so the pool's
// atomic alloc/free can be unit-tested against the real code (BRO-160).

static constexpr uint32_t FUTEX_BITSET_MATCH_ANY = 0xFFFFFFFFu;

static FutexWaiter* g_futexBuckets[FUTEX_HASH_SIZE];
// BRO-176: this lock MUST disable interrupts while held. It is a plain spinlock
// guarding the futex hash table; the critical sections are short and never
// block. Previously it was a bare test_and_set (interrupts stayed enabled), so a
// thread could be preempted by the timer while holding it and then be marked
// Terminated by a sibling's exit_group before reaching the release — leaking the
// lock permanently and deadlocking every future futex op (observed: 3 CPUs
// spinning here forever under schedstress). An IrqSpinLock keeps IF=0 while held,
// so the holder runs to the release uninterrupted and can never be descheduled
// or killed mid-section.
static IrqSpinLock g_futexLock;
// Defined in syscall_futex_pi.inc; all graph inspection uses g_futexLock.
static bool PiFutexQueued(uint64_t domain, uint64_t offset);
static void PiFutexCancelProcess(Process* proc);

// Pool of waiter nodes (avoid kmalloc from IRQ context). Sized to MAX_PROCESSES:
// a thread blocked in a futex wait cannot start another, so one slot per thread
// is the true upper bound and the pool can never be legitimately exhausted
// (BRO-160 — the old 128 cap returned -ENOMEM under heavy threading). Slot
// claim/release is atomic, so it is safe across FutexWake's lock drop window.
static constexpr uint32_t FUTEX_MAX_WAITERS = MAX_PROCESSES;
static FutexWaiterPool<FUTEX_MAX_WAITERS> g_futexWaiterPool;

static FutexWaiter* FutexAllocWaiter()
{
    return g_futexWaiterPool.Alloc();
}

static void FutexFreeWaiter(FutexWaiter* w)
{
    // futex_waitv nodes are on their waiter's kernel stack, not in the pool.
    // Never let the pool range-check a foreign pointer: its index truncation
    // could alias a live slot.
    if (w && !w->multi) g_futexWaiterPool.Free(w);
}

// BRO-196 diagnostic: a small ring buffer of recent futex ops, so the hang dump
// can show the WAIT/WAKE timeline right before a deadlock (lost-wakeup hunt).
struct FutexLogEntry { uint64_t uaddr; uint64_t tick; uint32_t pid; int32_t result; uint8_t op; };
static constexpr uint32_t FUTEX_LOG_SIZE = 512;
static FutexLogEntry g_futexLog[FUTEX_LOG_SIZE];
static volatile uint32_t g_futexLogIdx = 0;  // monotonic; & (SIZE-1) for slot
// op: 0=WAIT-enqueue 1=WAIT-return 2=WAKE 3=WAKE_OP 4=REQUEUE 5=WAITV-return
static inline void FutexLog(uint8_t op, uint64_t uaddr, uint32_t pid, int32_t result)
{
    uint32_t i = __atomic_fetch_add(&g_futexLogIdx, 1, __ATOMIC_RELAXED) & (FUTEX_LOG_SIZE - 1);
    g_futexLog[i].op = op;
    g_futexLog[i].uaddr = uaddr;
    g_futexLog[i].pid = pid;
    g_futexLog[i].result = result;
    g_futexLog[i].tick = ApicTickCount();
}

// BRO-196 diagnostic: dump every blocked futex waiter (uaddr + owner + pid).
// Called from SchedulerDumpHang (Ctrl+Shift+ScrollLock). Lock-free read of the
// hash table — safe in the hang path where we can't guarantee lock state; we are
// only reading immutable-while-blocked fields for diagnosis.
extern "C" void FutexDumpWaiters()
{
    SerialPrintf("--- futex waiters ---\n");
    uint32_t total = 0;
    for (uint32_t b = 0; b < FUTEX_HASH_SIZE; ++b) {
        for (FutexWaiter* w = g_futexBuckets[b]; w; w = w->next) {
            Process* wp = w->proc;
            uint32_t pid = (wp && wp->magic == PROCESS_MAGIC) ? wp->pid : 0xFFFFFFFFu;
            SerialPrintf("  futex uaddr=0x%lx owner=0x%lx bitset=0x%x pid=%u\n",
                         w->uaddr, w->owner, w->bitset, pid);
            ++total;
            if (total > 256) { SerialPrintf("  ...(truncated)\n"); return; }
        }
    }
    SerialPrintf("  total futex waiters: %u\n", total);

    // Recent futex op timeline (oldest→newest).
    static const char* opName[] = {"WAITq", "WAITr", "WAKE ", "WAKEOP", "REQ  ", "WAITV"};
    SerialPrintf("--- recent futex ops (last %u) ---\n", FUTEX_LOG_SIZE);
    uint32_t head = __atomic_load_n(&g_futexLogIdx, __ATOMIC_RELAXED);
    for (uint32_t k = 0; k < FUTEX_LOG_SIZE; ++k) {
        uint32_t i = (head + k) & (FUTEX_LOG_SIZE - 1);
        const FutexLogEntry& e = g_futexLog[i];
        if (e.uaddr == 0 && e.tick == 0) continue;
        const char* nm = (e.op < 6) ? opName[e.op] : "?";
        SerialPrintf("  t=%lu %s uaddr=0x%lx pid=%u res=%d\n",
                     e.tick, nm, e.uaddr, e.pid, e.result);
    }
}

// /proc/<pid>/futex: every queued futex node of that task or thread group,
// with the waiter's expected value beside the word's current value (read
// through the frame captured at enqueue). current != expected on a long
// wait means a wake did not reach this key; equal means nobody signalled.
extern "C" uint32_t FutexSnapshotTask(uint32_t pid, FutexTaskSnapshot* out, uint32_t capacity)
{
    uint32_t count = 0;
    const uint64_t flags = IrqSpinLockAcquire(&g_futexLock);
    for (uint32_t b = 0; b < FUTEX_HASH_SIZE; ++b) {
        for (FutexWaiter* w = g_futexBuckets[b]; w; w = w->next) {
            Process* p = w->proc;
            if (!p || p->magic != PROCESS_MAGIC || (p->pid != pid && p->tgid != pid)) continue;
            if (count < capacity) {
                FutexTaskSnapshot& e = out[count];
                e.tid = p->pid; e.multi = w->multi; e.expected = w->expected;
                e.userAddress = w->userAddress; e.owner = w->owner; e.key = w->uaddr;
                e.current = w->physical
                    ? __atomic_load_n(reinterpret_cast<volatile uint32_t*>(PhysToVirt(PhysicalAddress(w->physical)).raw()), __ATOMIC_RELAXED)
                    : 0xffffffffu;
            }
            ++count;
        }
    }
    IrqSpinLockRelease(&g_futexLock, flags);
    return count;
}

static uint32_t FutexHash(uint64_t owner, uint64_t addr)
{
    return static_cast<uint32_t>(((addr >> 2) ^ owner) % FUTEX_HASH_SIZE);
}

// Callable from outside syscall dispatch (e.g., scheduler thread exit).
// Only wakes waiters whose stored bitset overlaps wake_bitset.
// Pass FUTEX_BITSET_MATCH_ANY for plain FUTEX_WAKE.
extern "C" int64_t FutexWake(uint64_t owner, uint64_t uaddr, uint32_t maxWake,
                              uint32_t wake_bitset = FUTEX_BITSET_MATCH_ANY)
{
    uint32_t bucket = FutexHash(owner, uaddr);
    uint32_t woken = 0;

    uint64_t fxFlags = IrqSpinLockAcquire(&g_futexLock);

    if (PiFutexQueued(owner, uaddr)) {
        IrqSpinLockRelease(&g_futexLock, fxFlags);
        return -EINVAL; // Ordinary wake must not grant a PI ownership wait.
    }

    FutexWaiter** pp = &g_futexBuckets[bucket];
    while (*pp && woken < maxWake) {
        FutexWaiter* w = *pp;
        if (w->uaddr == uaddr && w->owner == owner && (w->bitset & wake_bitset) != 0) {
            Process* waiter = w->proc;
            *pp = w->next;
            w->queued = false;
            // A futex_waitv node lives on the waiter's kernel stack and may
            // vanish once the lock drops; FutexFreeWaiter leaves it alone.
            FutexFreeWaiter(w);
            // Unlink transfers the node's hold to this stack. Keep IF clear
            // until that hold is dropped: a killed caller never resumes.
            IrqSpinLockRelease(&g_futexLock, fxFlags & ~0x200ULL);

#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
            ProfilerTraceClientWait(waiter, FrameFutexWake, waiter->incarnation);
#endif
            WakeProcess(waiter);
            // The dequeued node hands its reference to this wake operation,
            // keeping the process alive across the unlocked wakeup window.
            SchedulerReleaseWaitReference(waiter);
            if (fxFlags & 0x200) __asm__ volatile("sti" ::: "memory");
            woken++;

            fxFlags = IrqSpinLockAcquire(&g_futexLock);
            pp = &g_futexBuckets[bucket];
        } else {
            pp = &(*pp)->next;
        }
    }

    IrqSpinLockRelease(&g_futexLock, fxFlags);
    { Process* cp = SchedulerCurrentProcess();
      FutexLog(2, uaddr, cp ? cp->pid : 0, (int32_t)woken); }
    return static_cast<int64_t>(woken);
}

static bool FutexRemoveWaiter(uint64_t owner, uint64_t uaddr, Process* proc)
{
    if (!proc) return false;
    uint32_t bucket = FutexHash(owner, uaddr);

    uint64_t fxFlags = IrqSpinLockAcquire(&g_futexLock);

    FutexWaiter** pp = &g_futexBuckets[bucket];
    while (*pp) {
        FutexWaiter* w = *pp;
        if (w->uaddr == uaddr && w->owner == owner && w->proc == proc && !w->multi) {
            *pp = w->next;
            w->queued = false;
            FutexFreeWaiter(w);
            IrqSpinLockRelease(&g_futexLock, fxFlags & ~0x200ULL);
            SchedulerReleaseWaitReference(proc);
            if (fxFlags & 0x200) __asm__ volatile("sti" ::: "memory");
            return true;
        }
        pp = &(*pp)->next;
    }

    IrqSpinLockRelease(&g_futexLock, fxFlags);
    return false;
}

// A coarse tick can wake us before the selected-clock deadline. Keep the
// existing queue entry/reference while rearming: removing and re-enqueuing
// would create a gap in which FUTEX_WAKE could be lost.
static bool FutexRearmWaiter(uint64_t owner, uint64_t uaddr, Process* proc,
                             const FutexWaitDeadline& deadline)
{
    const uint64_t flags = IrqSpinLockAcquire(&g_futexLock);
    for (FutexWaiter* w = g_futexBuckets[FutexHash(owner, uaddr)]; w; w = w->next) {
        if (w->owner != owner || w->uaddr != uaddr || w->proc != proc || w->multi) continue;
        const uint64_t nowNs = ClockNowNs(deadline.clockId);
        const uint64_t nowTick = ApicTickCount();
        proc->wakeupTick = deadline.wakeupTick(nowNs, nowTick);
        // Do not clear pendingWakeup here. A real wake after unlocking must
        // still prevent the next block through SchedulerBlock's wake latch.
        IrqSpinLockRelease(&g_futexLock, flags);
        return true;
    }
    IrqSpinLockRelease(&g_futexLock, flags);
    return false; // A real wake already removed the waiter.
}

// Caller owns a process reference and has observed it Terminated/off-CPU.
// Such a syscall cannot resume to remove its own waiter. Release only nodes
// still linked here; a concurrent wake owns any node it already dequeued.
extern "C" void FutexCancelProcess(Process* proc)
{
    uint64_t flags = IrqSpinLockAcquire(&g_futexLock);
    for (uint32_t b = 0; b < FUTEX_HASH_SIZE; ++b) {
        FutexWaiter** pp = &g_futexBuckets[b];
        while (*pp) {
            FutexWaiter* w = *pp;
            if (w->proc != proc) { pp = &w->next; continue; }
            *pp = w->next;
            w->queued = false;
            FutexFreeWaiter(w);
            ProcessUnref(proc); // caller's reference prevents final teardown
        }
    }
    IrqSpinLockRelease(&g_futexLock, flags);
    PiFutexCancelProcess(proc);
}

static int64_t FutexTimeoutDeadline(int op, uint64_t opVal, uint64_t timeoutAddr,
                                    FutexWaitDeadline& deadline)
{
    deadline = {};
    if (timeoutAddr == 0) return 0;
    timespec request{};
    if (!CopyFromUser(&request, timeoutAddr, sizeof(request))) return -EFAULT;
    uint64_t valueNs = 0;
    if (!FutexTimeoutNanoseconds(request.tv_sec, request.tv_nsec, &valueNs)) return -EINVAL;
    const bool relative = op == FUTEX_WAIT;
    const unsigned clockId = !relative && (opVal & FUTEX_CLOCK_REALTIME) ? 0 : 1;
    return FutexMakeDeadline(relative, valueNs, ClockNowNs(clockId), clockId, &deadline)
        ? 0 : -EINVAL;
}

// Shared memfd/file monotonic IDs are value identities, not an allocator or
// physical-page address. WAIT keeps its mapping transaction through comparison
// and queue insertion, then releases both locks before sleeping. Existing
// aliases keep the object alive; final unmap cannot reuse this key for a new
// object. Waiter cancellation retains only its existing Process reference.
static int FutexMappedKey(Process* process,uint64_t address,uint64_t operation,
                          const MappingRange* range,uint64_t& owner,uint64_t& keyAddress)
{
    owner=process?process->tgid:0;keyAddress=address;
    if(address&3)return -EINVAL;
    if((operation&FUTEX_PRIVATE_FLAG) || !range ||
       range->sharing!=MappingSharing::Shared || !range->backing)return 0;
    constexpr uint64_t SharedDomain=uint64_t{1}<<63;
    constexpr uint64_t FileDomain=uint64_t{1}<<62;
    uint64_t identity=0,domain=SharedDomain;
    if(range->kind==MappingKind::MemFd) {
        auto* memory=static_cast<MemFdData*>(range->backing->object);
        if(memory)identity=memory->inode;
    } else if(range->kind==MappingKind::File && range->backing->file) {
        identity=range->backing->file->orderId;
        domain|=FileDomain;
    } else return 0;
    // Reject exhaustion before publishing a key in the tagged owner domain.
    if(!identity || identity>=FileDomain)return -75;
    const uint64_t displacement=address-range->begin;
    if(range->offset>~uint64_t{0}-displacement)return -75;
    owner=domain|identity;keyAddress=range->offset+displacement;
    return 0;
}
static int FutexResolveKey(Process* process,uint64_t address,uint64_t operation,
                           uint64_t& owner,uint64_t& keyAddress)
{
    owner=process?process->tgid:0;keyAddress=address;
    if(address&3)return -EINVAL;
    if((operation&FUTEX_PRIVATE_FLAG) || !process || !process->addressSpace)return 0;
    MappingTransaction transaction(process,process->addressSpace);
    if(!transaction.LockFaultRange(address))return -ENOMEM;
    return FutexMappedKey(process,address,operation,
        MappingRangeFind(process->addressSpace->ranges,address),owner,keyAddress);
}

// Target-AS access never switches CR3 or dereferences the target's virtual
// address in the cleanup worker. Fault preparation precedes publication locks.
static bool RobustEnsurePage(Process* actor,AddressSpaceState* space,uint64_t address,uint32_t access)
{
    auto state=UserMappingPageAccess(actor,space,address,access,true);
    if(state==UserPageAccess::Missing) {
        if(ResolveMappingFaultForSpace(actor,space,address,4|((access&2)?2:0),8)!=MappingFaultResult::Resolved)return false;
        state=UserMappingPageAccess(actor,space,address,access,true);
    }
    return state==UserPageAccess::Ready;
}
static bool RobustRead(Process* actor,AddressSpaceState* space,uint64_t address,void* output,uint64_t length)
{
    constexpr uint64_t End=0x0000800000000000ULL;
    if(address<VIRTUAL_NULL_GUARD || address>=End || length>End-address)return false;
    auto* bytes=static_cast<uint8_t*>(output);
    while(length) {
        uint64_t count=4096-(address&4095);if(count>length)count=length;
        if(!RobustEnsurePage(actor,space,address,1))return false;
        MappingTransaction mapping(actor,space);
        if(!mapping.LockFaultRange(address))return false;
        const auto* range=MappingRangeFind(space->ranges,address);
        const uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(address));
        if(!range || range->kind==MappingKind::Device || !(range->protection&1) || range->end-address<count ||
           !(leaf&VMM_PRESENT) || !(leaf&VMM_USER))return false;
        const auto physical=VmmVirtToPhys(space->pageTable,VirtualAddress(address));
        if(!physical)return false;
        memcpy(bytes,reinterpret_cast<const void*>(PhysToVirt(physical).raw()),count);
        address+=count;bytes+=count;length-=count;
    }
    return true;
}
#include "syscall_futex_pi.inc"

static bool RobustWord(Process* actor,Process* target,AddressSpaceState* space,uint64_t address,bool pending)
{
    constexpr uint32_t OwnerMask=0x3fffffff,Waiters=0x80000000,Died=0x40000000;
    const uint32_t targetTid = ProcessVisibleTid(target, ProcessPidView(target));
    if(address&3)return false;
    uint32_t observed=0;
    if(!RobustRead(actor,space,address,&observed,4))return false;
    if((observed&OwnerMask)!=targetTid && !(pending && !(observed&OwnerMask)))return true;
    for(unsigned retry=0;retry<16;++retry) {
        const bool released=pending && !(observed&OwnerMask);
        const uint32_t access=released?1u:2u;
        if(!RobustEnsurePage(actor,space,address,access))return false;
        uint64_t owner=0,key=0;bool wake=false,done=false;
        {
            MappingTransaction mapping(actor,space);
            if(!mapping.LockFaultRange(address))return false;
            const auto* range=MappingRangeFind(space->ranges,address);
            const uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(address));
            if(!range || range->kind==MappingKind::Device || (range->protection&access)!=access || range->end-address<4 ||
               !(leaf&VMM_PRESENT) || !(leaf&VMM_USER) || ((access&2) && !(leaf&VMM_WRITABLE)))continue;
            if(FutexMappedKey(target,address,0,range,owner,key))return false;
            const auto physical=VmmVirtToPhys(space->pageTable,VirtualAddress(address));
            if(!physical)return false;
            auto* word=reinterpret_cast<uint32_t*>(PhysToVirt(physical).raw());
            const uint64_t flags=IrqSpinLockAcquire(&g_futexLock);
            observed=__atomic_load_n(word,__ATOMIC_ACQUIRE);
            if(pending && !(observed&OwnerMask)) {wake=true;done=true;}
            else if((observed&OwnerMask)!=targetTid)done=true;
            else if(access&2) {
                const uint32_t replacement=(observed&Waiters)|Died;
                wake=(observed&Waiters)!=0;
                done=__atomic_compare_exchange_n(word,&observed,replacement,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
            }
            IrqSpinLockRelease(&g_futexLock,flags);
        }
        // No mapping/file lock may survive into scheduler wake operations.
        if(done) {
            if(wake && !FutexWake(owner,key,1) && (owner>>63))
                (void)FutexWake(target->tgid,address,1);
            return true;
        }
    }
    return false;
}
void RobustFutexCleanup(Process* target)
{
    if(!target || target->isKernelThread)return;
    auto* actor=ProcessCurrent();if(!actor)return;
    KernelOperationGuard protectedActor(actor);
    uint32_t state=__atomic_load_n(&target->robustExitState,__ATOMIC_ACQUIRE);
    for(;;) {
        if(state>=2)return;
        if(__atomic_compare_exchange_n(&target->robustExitState,&state,2,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
    }
    auto* space=target->addressSpace;
    AddressSpaceRef(space);
    const uint64_t head=__atomic_exchange_n(&target->robustListHead,0,__ATOMIC_ACQ_REL);
    struct Head {uint64_t next;int64_t offset;uint64_t pending;} list{};
    if(head && space && !space->kernel && RobustRead(actor,space,head,&list,sizeof(list))) {
        auto address=[&](uint64_t entry,uint64_t& result) {
            const uint64_t raw=entry&~uint64_t{1};
            if(list.offset>=0)return !__builtin_add_overflow(raw,static_cast<uint64_t>(list.offset),&result);
            const uint64_t distance=uint64_t{0}-static_cast<uint64_t>(list.offset);
            if(raw<distance)return false;result=raw-distance;return true;
        };
        uint64_t entry=list.next;
        for(unsigned count=0;count<2048 && (entry&~uint64_t{1})!=head;++count) {
            const uint64_t node=entry&~uint64_t{1};uint64_t next=0,word=0;
            const bool gotNext=RobustRead(actor,space,node,&next,8);
            if(node!=(list.pending&~uint64_t{1}) &&
               (!address(entry,word) || !RobustWord(actor,target,space,word,false)))break;
            if(!gotNext)break;
            entry=next;
        }
        uint64_t word=0;
        if(list.pending && address(list.pending,word))
            (void)RobustWord(actor,target,space,word,true);
    }
    // Read every dying-owner list link before waking a PI successor: glibc
    // immediately reuses the acquired mutex's embedded next pointer when it
    // links that mutex into the successor's robust list.
    PiFutexOwnerExit(target);
    AddressSpaceUnref(space);
    __atomic_store_n(&target->robustExitState,3,__ATOMIC_RELEASE);
}

// /proc/<pid>/shmframes (diagnostic): every shared file/memfd mapping of a
// task with its futex key identity and, per page, a frame tag and leaf state.
// Equal tags across processes mean they map the same physical frame for that
// object offset; different tags at one key+offset mean incoherent views. Tags
// are a boot-keyed permutation of the frame number, never a raw address.
namespace {
struct TextOut {
    char* p; char* end;
    void Put(const char* s) { while (*s && p < end) *p++ = *s++; }
    void Hex(uint64_t v) {
        char tmp[16]; int n = 0;
        do { tmp[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && n < 16);
        while (n && p < end) *p++ = tmp[--n];
    }
    void Dec(uint64_t v) {
        char tmp[20]; int n = 0;
        do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && n < 20);
        while (n && p < end) *p++ = tmp[--n];
    }
};
uint64_t FrameTag(uint64_t frame)
{
    static uint64_t key;
    uint64_t k = __atomic_load_n(&key, __ATOMIC_RELAXED);
    if (!k) {
        uint64_t fresh = ApicTickCount() * 0x9e3779b97f4a7c15ull ^ 0xd1b54a32d192ed03ull;
        fresh |= 1;
        __atomic_compare_exchange_n(&key, &k, fresh, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        k = __atomic_load_n(&key, __ATOMIC_RELAXED);
    }
    uint64_t x = (frame ^ (k >> 7)) * k; // odd multiplier: a bijection on frames
    return (x ^ (x >> 29)) & 0xffffffffffull;
}
} // namespace

extern "C" uint32_t FutexDescribeSharedMappings(uint16_t pid, uint64_t incarnation, char* buffer, uint32_t capacity)
{
    if (!buffer || capacity < 2) return 0;
    TextOut out{buffer, buffer + capacity - 1};
    Process* actor = ProcessCurrent();
    Process* live = SchedulerFindProcessReference(pid);
    if (!live) return 0;
    AddressSpaceState* space = live->incarnation == incarnation ? live->addressSpace : nullptr;
    if (space && !space->kernel && actor) {
        AddressSpaceRef(space);
        {
            MappingTransaction mapping(actor, space);
            if (mapping.Lock()) {
                for (auto* node = space->ranges.head; node; node = node->next) {
                    const MappingRange& range = node->range;
                    if (range.sharing != MappingSharing::Shared || !range.backing ||
                        (range.kind != MappingKind::File && range.kind != MappingKind::MemFd)) continue;
                    uint64_t owner = 0, key = 0;
                    if (FutexMappedKey(live, range.begin, 0, &range, owner, key)) continue;
                    out.Put(range.kind == MappingKind::File ? "file:" : "memfd:");
                    out.Dec(owner & ((uint64_t{1} << 62) - 1));
                    out.Put(" va=0x"); out.Hex(range.begin); out.Put("-0x"); out.Hex(range.end);
                    out.Put(" offset=0x"); out.Hex(range.offset);
                    out.Put(" prot="); out.Put(range.protection & 1 ? "r" : "-");
                    out.Put(range.protection & 2 ? "w" : "-"); out.Put("\n");
                    const uint64_t pages = (range.end - range.begin) / 4096;
                    for (uint64_t i = 0; i < pages && i < 32; ++i) {
                        const uint64_t va = range.begin + i * 4096;
                        const uint64_t leaf = VmmReadLeaf(space->pageTable, VirtualAddress(va));
                        if (!(leaf & VMM_PRESENT)) continue;
                        out.Put("  +0x"); out.Hex(range.offset + i * 4096);
                        out.Put(" frame="); out.Hex(FrameTag(VmmLeafPhysical(leaf).raw() >> 12));
                        out.Put(leaf & VMM_WRITABLE ? " rw" : " ro");
                        if (leaf & PTE_COW_BIT) out.Put(" cow");
                        out.Put("\n");
                    }
                    if (pages > 32) { out.Put("  ... "); out.Dec(pages - 32); out.Put(" more pages\n"); }
                }
            } else out.Put("(address space busy)\n");
        }
        AddressSpaceUnref(space);
    }
    ProcessUnref(live);
    *out.p = 0;
    return static_cast<uint32_t>(out.p - buffer);
}

int64_t sys_futex(uint64_t uaddrVal, uint64_t opVal, uint64_t val,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    int op = static_cast<int>(opVal) & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    if (op == FUTEX_LOCK_PI || op == FUTEX_UNLOCK_PI || op == FUTEX_TRYLOCK_PI || op == FUTEX_LOCK_PI2)
        return PiFutexOperation(uaddrVal,opVal,op,arg4);
    // PI condition-variable requeue needs its own atomic source/destination
    // admission protocol. It remains honestly unsupported in this first stage.
    if (op == FUTEX_WAIT_REQUEUE_PI || op == FUTEX_CMP_REQUEUE_PI)
        return -ENOSYS;
    Process* proc = ProcessCurrent();
    // Private addresses keep the existing per-thread-group key. Non-private
    // shared memfd aliases need an object/offset key across different VAs/TGIDs.
    uint64_t owner=0,keyAddress=0;
    owner=proc?proc->tgid:0;keyAddress=uaddrVal;
    if(uaddrVal&3)return -EINVAL;
    if(op!=FUTEX_WAIT && op!=FUTEX_WAIT_BITSET)
        if(int error=FutexResolveKey(proc,uaddrVal,opVal,owner,keyAddress))return error;
    if((owner>>63) && op!=FUTEX_WAKE && op!=FUTEX_WAKE_BITSET && op!=FUTEX_WAKE_OP)return -ENOSYS;

    if (op == FUTEX_WAKE || op == FUTEX_WAKE_BITSET) {
        uint32_t maxWake = static_cast<uint32_t>(val);
        if (maxWake == 0) return 0;
        uint32_t wake_bitset = (op == FUTEX_WAKE_BITSET)
            ? static_cast<uint32_t>(arg6)
            : FUTEX_BITSET_MATCH_ANY;
        if (wake_bitset == 0) return -EINVAL;
        int64_t r = FutexWake(owner, keyAddress, maxWake, wake_bitset);
        return r;
    }

    if (op == FUTEX_WAKE_OP) {
        uint64_t secondOwner=0,secondAddress=0;
        if(int error=FutexResolveKey(proc,arg5,opVal,secondOwner,secondAddress))return error;
        // Shared WAKE_OP still needs two-key mapping admission work.
        if((owner>>63) || (secondOwner>>63))return -95;
        if (!UserBufferWritable(arg5, sizeof(uint32_t)))
            return -EFAULT;
        uint32_t encoded = static_cast<uint32_t>(arg6);
        uint32_t opCode = (encoded >> 28) & 0xF;
        uint32_t cmpCode = (encoded >> 24) & 0xF;
        uint32_t opArg = (encoded >> 12) & 0xFFF;
        uint32_t cmpArg = encoded & 0xFFF;
        if (opArg & 0x800) opArg |= 0xFFFFF000u;
        if (cmpArg & 0x800) cmpArg |= 0xFFFFF000u;
        if (opCode & 0x8) {
            opCode &= 0x7;
            if (opArg >= 32) return -EINVAL;
            opArg = 1u << opArg;
        }

        if (opCode > 4 || cmpCode > 5) return -ENOSYS;
        if (!RobustEnsurePage(proc,proc->addressSpace,arg5,3)) return -EFAULT;
        uint32_t oldVal = 0;
        {
            MappingTransaction mapping(proc,proc->addressSpace);
            if (!mapping.LockFaultRange(arg5)) return -ENOMEM;
            auto* uaddr2=PiMappedWord(proc->addressSpace,arg5);
            if (!uaddr2) return -EFAULT;
            const auto flags=IrqSpinLockAcquire(&g_futexLock);
            if (PiFutexQueued(owner,keyAddress) || PiFutexQueued(secondOwner,secondAddress)) {
                IrqSpinLockRelease(&g_futexLock,flags);return -EINVAL;
            }
            switch (opCode) {
            case 0: oldVal=__atomic_exchange_n(uaddr2,opArg,__ATOMIC_ACQ_REL);break;
            case 1: oldVal=__atomic_fetch_add(uaddr2,opArg,__ATOMIC_ACQ_REL);break;
            case 2: oldVal=__atomic_fetch_or(uaddr2,opArg,__ATOMIC_ACQ_REL);break;
            case 3: oldVal=__atomic_fetch_and(uaddr2,~opArg,__ATOMIC_ACQ_REL);break;
            case 4: oldVal=__atomic_fetch_xor(uaddr2,opArg,__ATOMIC_ACQ_REL);break;
            }
            IrqSpinLockRelease(&g_futexLock,flags);
        }

        bool cmp = false;
        int32_t lhs = static_cast<int32_t>(oldVal);
        int32_t rhs = static_cast<int32_t>(cmpArg);
        switch (cmpCode) {
        case 0: cmp = (lhs == rhs); break; // FUTEX_OP_CMP_EQ
        case 1: cmp = (lhs != rhs); break; // FUTEX_OP_CMP_NE
        case 2: cmp = (lhs <  rhs); break; // FUTEX_OP_CMP_LT
        case 3: cmp = (lhs <= rhs); break; // FUTEX_OP_CMP_LE
        case 4: cmp = (lhs >  rhs); break; // FUTEX_OP_CMP_GT
        case 5: cmp = (lhs >= rhs); break; // FUTEX_OP_CMP_GE
        default: return -ENOSYS;
        }

        uint32_t maxWake1 = static_cast<uint32_t>(val);
        uint32_t maxWake2 = static_cast<uint32_t>(arg4);
        int64_t woken = maxWake1 ? FutexWake(owner, keyAddress, maxWake1) : 0;
        if (woken < 0) return woken;
        if (cmp && maxWake2) {
            const auto second=FutexWake(secondOwner, secondAddress, maxWake2);
            if (second < 0) return second;
            woken += second;
        }
        return woken;
    }

    if (op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET) {
        if (!UserBufferReadable(uaddrVal, sizeof(uint32_t)))
            return -EFAULT;
        auto* uaddr = reinterpret_cast<volatile uint32_t*>(uaddrVal);

        if (!proc) {
            SerialPrintf("sys_futex: WAIT no current process!\n");
            return -ENOSYS;
        }

        FutexWaitDeadline deadline;
        if (int64_t error = FutexTimeoutDeadline(op, opVal, arg4, deadline)) return error;
        const uint32_t waitBitset = op == FUTEX_WAIT_BITSET
            ? static_cast<uint32_t>(arg6) : FUTEX_BITSET_MATCH_ANY;
        if (!waitBitset) return -EINVAL;

        // Fault safely before taking mapping/futex locks. The mapping
        // transaction then keeps key derivation, value comparison and queue
        // publication in one admission interval against unmap/remap.
        uint32_t prefault=0;
        if(!CopyFromUser(&prefault,uaddrVal,sizeof(prefault)))return -EFAULT;
        {
        MappingTransaction mapping(proc,proc->addressSpace);
        if(!mapping.LockFaultRange(uaddrVal))return -ENOMEM;
        const auto* range=MappingRangeFind(proc->addressSpace->ranges,uaddrVal);
        if(!range || !(range->protection&1) || uaddrVal>range->end || range->end-uaddrVal<4)return -EFAULT;
        // A concurrent remap after prefault may install a new lazy range.
        // Never fault while holding AS mutation or the IRQ futex lock: let
        // userspace retry admission instead of touching an absent leaf.
        const uint64_t leaf=VmmReadLeaf(proc->addressSpace->pageTable,VirtualAddress(uaddrVal));
        if(!(leaf&VMM_PRESENT) || !(leaf&VMM_USER))return -EAGAIN;
        if(int error=FutexMappedKey(proc,uaddrVal,opVal,range,owner,keyAddress))return error;
        uint64_t fxFlags = IrqSpinLockAcquire(&g_futexLock);

        if (PiFutexQueued(owner, keyAddress)) {
            IrqSpinLockRelease(&g_futexLock, fxFlags);
            return -EINVAL;
        }

        // Check if *uaddr == val while holding the lock
        if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != static_cast<uint32_t>(val)) {
            IrqSpinLockRelease(&g_futexLock, fxFlags);
            return -EAGAIN;
        }

        if (deadline.finite && deadline.expired(ClockNowNs(deadline.clockId))) {
            IrqSpinLockRelease(&g_futexLock, fxFlags);
            return -ETIMEDOUT;
        }

        // Allocate and enqueue waiter
        FutexWaiter* w = FutexAllocWaiter();
        if (!w) {
            IrqSpinLockRelease(&g_futexLock, fxFlags);
            SerialPrintf("sys_futex: ENOMEM (pool exhausted) pid=%u\n", proc->pid);
            return -ENOMEM;
        }
        w->uaddr = keyAddress;
        w->owner = owner;
        w->proc = proc;
        w->bitset = waitBitset;
        w->queued = true;
        w->multi = false;
        w->expected = static_cast<uint32_t>(val);
        w->userAddress = uaddrVal;
        w->physical = VmmVirtToPhys(proc->addressSpace->pageTable, VirtualAddress(uaddrVal)).raw();
        uint32_t bucket = FutexHash(owner, keyAddress);
        ProcessRef(proc); // owned by linked waiter, then wake/remove/cancel
        w->next = g_futexBuckets[bucket];
        g_futexBuckets[bucket] = w;

        // Clear pending wakeup flag before blocking
        __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);
        if (deadline.finite) {
            // Sample the ns clock first, then its coarse scheduling hint.
            const uint64_t nowNs = ClockNowNs(deadline.clockId);
            const uint64_t nowTick = ApicTickCount();
            proc->wakeupTick = deadline.wakeupTick(nowNs, nowTick);
        } else {
            proc->wakeupTick = ~0ULL;
        }

        // Release the futex lock (restoring interrupts) BEFORE blocking — a
        // process must never be descheduled with interrupts disabled.
        IrqSpinLockRelease(&g_futexLock, fxFlags);
        } // release mapping transaction, backing pins and AS gates before wait
        FutexLog(0, uaddrVal, proc->pid, (int32_t)val);  // WAIT-enqueue

        for (;;) {
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
            ProfilerTraceClientWait(proc, FrameWaitBegin, FrameWaitFutex, op);
#endif
            SchedulerBlockInterruptible(proc);
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
            ProfilerTraceClientWait(proc, FrameWaitEnd, FrameWaitFutex, op);
#endif
            const bool interrupted = HasPendingSignals();
            const bool expired = deadline.finite && deadline.expired(ClockNowNs(deadline.clockId));
            if (interrupted || expired || !deadline.finite) {
                if (!FutexRemoveWaiter(owner, keyAddress, proc)) break;
                if (interrupted) return -EINTR;
                if (expired) return -ETIMEDOUT;
                // BRO-205: an infinite wait may return spuriously, but must
                // never report ETIMEDOUT (glibc mutex waits abort on it).
                return 0;
            }
            // Finite early wake: retain the entry so concurrent FUTEX_WAKE
            // cannot disappear between a dequeue and a new enqueue.
            if (!FutexRearmWaiter(owner, keyAddress, proc, deadline)) break;
        }

        FutexLog(1, uaddrVal, proc->pid, 0);  // WAIT-return (woken)
        return 0;
    }

    {
        uint64_t userRip = 0, userRax = 0;
        __asm__ volatile("movq %%gs:48, %0" : "=r"(userRip));
        __asm__ volatile("movq %%gs:120, %0" : "=r"(userRax));
        SerialPrintf("sys_futex: unsupported op=%d (raw=0x%lx) pid=%u rax=%lu rip=0x%lx "
                     "uaddr=0x%lx val=0x%lx a4=0x%lx a5=0x%lx a6=0x%lx\n",
                     op, opVal,
                     ProcessCurrent() ? ProcessCurrent()->pid : 0,
                     userRax, userRip, uaddrVal, val, arg4, arg5, arg6);
    }
    return -ENOSYS;
}

// ---------------------------------------------------------------------------
// sys_futex_waitv (449) — wait on up to 128 futexes (Linux 5.16 futex2).
// ---------------------------------------------------------------------------
// Queue before check, as Linux does: each node is linked under the futex lock
// before its word is read, so a store + FUTEX_WAKE racing the setup finds the
// node queued and cannot be lost. Keys match sys_futex exactly (private TGID/VA
// or shared memfd/file object + offset), so classic FUTEX_WAKE reaches these
// nodes. Nodes live on this thread's kernel stack: each linked node owns one
// process reference and the reaper requires refCount==0, so no node outlives
// that stack; a killed waiter's nodes are unlinked by FutexCancelProcess.

// Unlink every still-queued node and return the highest index a waker took
// (Linux unqueue_multiple), or -1.
static int32_t FutexWaitvUnqueue(Process* proc, FutexWaiter* nodes, uint32_t count)
{
    uint32_t released = 0;
    const uint64_t flags = IrqSpinLockAcquire(&g_futexLock);
    const int32_t woken = FutexWaitvWokenIndex(count, [&](uint32_t i) { return nodes[i].queued; });
    for (uint32_t i = 0; i < count; ++i) {
        FutexWaiter* node = &nodes[i];
        if (!node->queued) continue;
        for (FutexWaiter** pp = &g_futexBuckets[FutexHash(node->owner, node->uaddr)]; *pp; pp = &(*pp)->next) {
            if (*pp != node) continue;
            *pp = node->next;
            node->queued = false;
            ++released;
            break;
        }
        if (node->queued) KernelPanic("futex_waitv: queued node missing from its bucket");
    }
    // As in FutexRemoveWaiter: keep IF clear until the node holds are dropped.
    IrqSpinLockRelease(&g_futexLock, flags & ~0x200ULL);
    while (released--) SchedulerReleaseWaitReference(proc);
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
    return woken;
}

// Admit one validated entry under the classic WAIT's mapping transaction:
// derive its key, link the node, then read the word while already queued.
// Returns 0 with the node linked (matched reports the comparison), or an
// error with the node not linked.
static int FutexWaitvQueueOne(Process* proc, FutexWaiter* node, const FutexWaitvEntry& entry, bool& matched)
{
    const uint64_t address = entry.uaddr;
    if (!UserBufferReadable(address, sizeof(uint32_t))) return -EFAULT;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t prefault = 0;
        if (!CopyFromUser(&prefault, address, sizeof(prefault))) return -EFAULT;
        MappingTransaction mapping(proc, proc->addressSpace);
        if (!mapping.LockFaultRange(address)) return -ENOMEM;
        const auto* range = MappingRangeFind(proc->addressSpace->ranges, address);
        if (!range || !(range->protection & 1) || address > range->end || range->end - address < 4) return -EFAULT;
        // Never fault under AS mutation or the IRQ futex lock. A remap after
        // the prefault leaves a lazy leaf: drop the transaction and refault.
        const uint64_t leaf = VmmReadLeaf(proc->addressSpace->pageTable, VirtualAddress(address));
        if (!(leaf & VMM_PRESENT) || !(leaf & VMM_USER)) continue;
        uint64_t owner = 0, key = 0;
        if (int error = FutexMappedKey(proc, address, entry.flags & FUTEX2_PRIVATE, range, owner, key)) return error;
        const uint64_t flags = IrqSpinLockAcquire(&g_futexLock);
        if (PiFutexQueued(owner, key)) {
            IrqSpinLockRelease(&g_futexLock, flags);
            return -EINVAL;
        }
        node->uaddr = key;
        node->owner = owner;
        node->proc = proc;
        node->bitset = FUTEX_BITSET_MATCH_ANY;
        node->multi = true;
        node->queued = true;
        node->expected = static_cast<uint32_t>(entry.val);
        node->userAddress = address;
        node->physical = VmmVirtToPhys(proc->addressSpace->pageTable, VirtualAddress(address)).raw();
        ProcessRef(proc); // owned by the linked node, then wake/unqueue/cancel
        const uint32_t bucket = FutexHash(owner, key);
        node->next = g_futexBuckets[bucket];
        g_futexBuckets[bucket] = node;
        matched = __atomic_load_n(reinterpret_cast<volatile uint32_t*>(address), __ATOMIC_ACQUIRE) ==
                  static_cast<uint32_t>(entry.val);
        IrqSpinLockRelease(&g_futexLock, flags);
        return 0;
    }
    return -EAGAIN;
}

int64_t sys_futex_waitv(uint64_t waitersAddr, uint64_t count, uint64_t flags,
                        uint64_t timeoutAddr, uint64_t clockid, uint64_t)
{
    if (int error = FutexWaitvCheckCall(waitersAddr, count, flags, timeoutAddr, clockid)) return error;
    Process* proc = ProcessCurrent();
    if (!proc || !proc->addressSpace) return -ENOSYS;

    // The timeout is absolute on the selected clock, like FUTEX_WAIT_BITSET.
    FutexWaitDeadline deadline{};
    if (timeoutAddr) {
        timespec request{};
        if (!CopyFromUser(&request, timeoutAddr, sizeof(request))) return -EFAULT;
        uint64_t valueNs = 0;
        if (!FutexTimeoutNanoseconds(request.tv_sec, request.tv_nsec, &valueNs)) return -EINVAL;
        const unsigned clockId = static_cast<unsigned>(clockid);
        if (!FutexMakeDeadline(false, valueNs, ClockNowNs(clockId), clockId, &deadline)) return -EINVAL;
    }

    FutexWaitvEntry entries[FUTEX_WAITV_MAX];
    if (!CopyFromUser(entries, waitersAddr, count * sizeof(FutexWaitvEntry))) return -EFAULT;
    for (uint32_t i = 0; i < count; ++i)
        if (int error = FutexWaitvCheckEntry(entries[i])) return error;

    FutexWaiter nodes[FUTEX_WAITV_MAX];
    for (uint32_t i = 0; i < count; ++i) nodes[i].queued = false;

    uint32_t linked = 0;
    int error = 0;
    bool mismatch = false;
    while (linked < count) {
        bool matched = false;
        error = FutexWaitvQueueOne(proc, &nodes[linked], entries[linked], matched);
        if (error) break;
        ++linked;
        if (!matched) { mismatch = true; break; }
    }
    if (error || mismatch) {
        // A wake that raced the setup wins over the mismatch, as in Linux.
        const int32_t woken = FutexWaitvUnqueue(proc, nodes, linked);
        if (woken >= 0) return woken;
        return error ? error : -EAGAIN;
    }

    bool interrupted = false;
    for (;;) {
        // Decide under the futex lock: a wake after this check unlinks a node
        // and latches pendingWakeup, so the block below returns at once.
        bool ready = false;
        {
            const uint64_t fxFlags = IrqSpinLockAcquire(&g_futexLock);
            const uint64_t nowNs = deadline.finite ? ClockNowNs(deadline.clockId) : 0;
            ready = FutexWaitvWokenIndex(count, [&](uint32_t i) { return nodes[i].queued; }) >= 0 ||
                    deadline.expired(nowNs);
            if (!ready) {
                __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);
                proc->wakeupTick = deadline.finite ? deadline.wakeupTick(nowNs, ApicTickCount()) : ~0ULL;
            }
            IrqSpinLockRelease(&g_futexLock, fxFlags);
        }
        if (ready) break;
        SchedulerBlockInterruptible(proc);
        if (HasPendingSignals()) { interrupted = true; break; }
        // Otherwise a timer hint or spurious wake: recheck while still queued.
    }

    const int32_t woken = FutexWaitvUnqueue(proc, nodes, count);
    FutexLog(5, entries[woken >= 0 ? woken : 0].uaddr, proc->pid, woken);
    if (woken >= 0) return woken;
    if (deadline.expired(deadline.finite ? ClockNowNs(deadline.clockId) : 0)) return -ETIMEDOUT;
    // Linux returns -ERESTARTSYS: SignalSyscallMayRestart lets SA_RESTART
    // replay it, which is exact because the timeout is absolute.
    (void)interrupted;
    return -EINTR;
}

} // namespace brook

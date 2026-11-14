#pragma once
#include <stdint.h>

namespace brook {
// VCS0 (BSD video engine) bring-up state, VA-API phase 1. See
// docs/bdw-video-engine.md. Everything here is gated twice: the build option
// BROOK_BDW_VIDEO_ENGINE (OFF by default) compiles the device paths in, and
// the runtime gate (`gpu video on|off`, boot default off) must be on before any
// VCS register is written. Nothing is advertised to userspace (HAS_BSD,
// I915_EXEC_BSD) until a submission self-test has passed on this boot.

// Gen8 VCS0 register block and GT reset domain.
constexpr uint32_t BdwVcsBase=0x12000;
constexpr uint32_t BdwVcsResetDomain=4;      // GEN6_GRDOM_MEDIA
constexpr uint64_t BdwVcsEnginePage=0xf6012000ull; // BAR0 + 0x12000 on the Dell

enum class BdwVideoStage : uint8_t {
    Off,          // gate off (boot default): no VCS access
    Enabled,      // gate on, not yet probed this boot
    Probed,       // read-only engine sample taken and looked sane
    ResetOk,      // a VCS domain reset was acknowledged
    SelftestOk,   // a VCS submission self-test completed and verified
    Quarantined,  // an operation failed in a way that leaves VCS state unknown
};

enum class BdwVideoRefusal : uint8_t {
    None, GateOff, Quarantined, Busy, ProbeFailed, NotQuiescent, ResetRefused, ResetUncertain, NotImplemented,
    PrepareFailed, SubmitFailed, Mismatch, RetireFailed,
};
inline const char* BdwVideoRefusalName(BdwVideoRefusal r){
    switch(r){
    case BdwVideoRefusal::None:return "none";
    case BdwVideoRefusal::GateOff:return "gate-off";
    case BdwVideoRefusal::Quarantined:return "quarantined";
    case BdwVideoRefusal::Busy:return "diagnostic-busy";
    case BdwVideoRefusal::ProbeFailed:return "probe-failed";
    case BdwVideoRefusal::NotQuiescent:return "vcs-not-quiescent";
    case BdwVideoRefusal::ResetRefused:return "reset-refused";
    case BdwVideoRefusal::ResetUncertain:return "reset-uncertain";
    case BdwVideoRefusal::NotImplemented:return "not-implemented";
    case BdwVideoRefusal::PrepareFailed:return "prepare-failed";
    case BdwVideoRefusal::SubmitFailed:return "submit-failed";
    case BdwVideoRefusal::Mismatch:return "readback-mismatch";
    case BdwVideoRefusal::RetireFailed:return "retire-failed";
    }
    return "unknown";
}
inline const char* BdwVideoStageName(BdwVideoStage s){
    switch(s){
    case BdwVideoStage::Off:return "off";
    case BdwVideoStage::Enabled:return "enabled";
    case BdwVideoStage::Probed:return "probed";
    case BdwVideoStage::ResetOk:return "reset-ok";
    case BdwVideoStage::SelftestOk:return "selftest-ok";
    case BdwVideoStage::Quarantined:return "quarantined";
    }
    return "unknown";
}

// One VCS0 sample (both reads of BdwObserveEngine's pair, first copy).
struct BdwVcsSample {
    bool sampled=false,idle=false,quiescent=false;
    uint32_t head=0,tail=0,mode=0,start=0,control=0,execlistLo=0,execlistHi=0,submissionMode=0;
};

// Pure policy: what is allowed next, and how results move the stage. Kept free
// of device access so it can be host-tested exhaustively.
class BdwVideoGate {
    BdwVideoStage stage_=BdwVideoStage::Off;
    BdwVideoRefusal last_=BdwVideoRefusal::None;
    uint32_t probes_=0,resets_=0,refusals_=0,selftests_=0,selftestPasses_=0,retries_=0;
    const char* detail_="none"; // static string: the precise sub-reason of the last refusal
    uint32_t rc6Pct_=0;bool rc6Resumed_=false,rc6Checked_=false;
    BdwVcsSample sample_{};
    void refuse(BdwVideoRefusal r){last_=r;++refusals_;}
public:
    BdwVideoStage stage()const{return stage_;}
    BdwVideoRefusal lastRefusal()const{return last_;}
    uint32_t probes()const{return probes_;}
    uint32_t resets()const{return resets_;}
    uint32_t refusals()const{return refusals_;}
    uint32_t selftests()const{return selftests_;}
    uint32_t retries()const{return retries_;}
    const char* detail()const{return detail_;}
    // RC6 residency over a short idle window right after the last reset or
    // self-test (gen 392: a media reset without the MAX_IDLE restore left RC6 at 0%).
    uint32_t rc6Pct()const{return rc6Pct_;}
    bool rc6Resumed()const{return rc6Resumed_;}
    bool rc6Checked()const{return rc6Checked_;}
    void setRc6(bool checked,uint32_t pct){rc6Checked_=checked;rc6Pct_=pct;rc6Resumed_=checked && pct>0;}
    void setDetail(const char* d){detail_=d?d:"none";}
    void addRetries(uint32_t n){retries_+=n;}
    uint32_t selftestPasses()const{return selftestPasses_;}
    const BdwVcsSample& sample()const{return sample_;}
    bool enabled()const{return stage_!=BdwVideoStage::Off && stage_!=BdwVideoStage::Quarantined;}
    // Turning the gate on is allowed unless quarantined (quarantine lasts the boot).
    bool setEnabled(bool on){
        if(stage_==BdwVideoStage::Quarantined){refuse(BdwVideoRefusal::Quarantined);return false;}
        stage_=on?(stage_==BdwVideoStage::Off?BdwVideoStage::Enabled:stage_):BdwVideoStage::Off;
        return true;
    }
    // A read-only probe needs only the gate.
    BdwVideoRefusal mayProbe()const{
        if(stage_==BdwVideoStage::Quarantined)return BdwVideoRefusal::Quarantined;
        if(stage_==BdwVideoStage::Off)return BdwVideoRefusal::GateOff;
        return BdwVideoRefusal::None;
    }
    void probed(bool ok,const BdwVcsSample& s){
        ++probes_;sample_=s;
        if(!ok || !s.sampled){refuse(BdwVideoRefusal::ProbeFailed);return;}
        if(stage_==BdwVideoStage::Enabled)stage_=BdwVideoStage::Probed;
    }
    // A reset needs a successful probe this boot that saw VCS quiescent: we
    // only take over an idle engine (IdleTakeover); nothing else submits to it.
    BdwVideoRefusal mayReset()const{
        if(stage_==BdwVideoStage::Quarantined)return BdwVideoRefusal::Quarantined;
        if(stage_==BdwVideoStage::Off)return BdwVideoRefusal::GateOff;
        if(stage_==BdwVideoStage::Enabled)return BdwVideoRefusal::ProbeFailed;
        if(!sample_.sampled || !sample_.quiescent)return BdwVideoRefusal::NotQuiescent;
        return BdwVideoRefusal::None;
    }
    // complete: domain reset acknowledged. uncertain: a write may have reached
    // the device without a verified outcome. Neither: refused before any write.
    void resetDone(bool complete,bool uncertain){
        ++resets_;
        if(uncertain){stage_=BdwVideoStage::Quarantined;refuse(BdwVideoRefusal::ResetUncertain);return;}
        if(!complete){refuse(BdwVideoRefusal::ResetRefused);return;}
        stage_=BdwVideoStage::ResetOk;
    }
    // The self-test resets VCS itself before submitting, so it needs the same
    // proof as a reset: a probe this boot that saw VCS idle and unowned.
    BdwVideoRefusal maySelftest()const{return mayReset();}
    // ok: completed, nonce and data verified, engine reset and lease retired.
    // A refusal before any engine write leaves the stage unchanged. Any failure
    // after the engine was touched quarantines VCS for the boot (the caller
    // panics instead only if the GT reset or PAT state itself is uncertain).
    void selftestDone(bool ok,bool engineTouched,BdwVideoRefusal reason){
        ++selftests_;
        if(ok){++selftestPasses_;stage_=BdwVideoStage::SelftestOk;return;}
        refuse(reason==BdwVideoRefusal::None?BdwVideoRefusal::SubmitFailed:reason);
        if(engineTouched)stage_=BdwVideoStage::Quarantined;
    }
    // User submission (execbuffer I915_EXEC_BSD) is phase 1c and not
    // implemented: userspace is never told VCS exists, even after a self-test.
    bool advertiseBsd()const{return false;}
};

// MI commands used by the planned VCS self-test (phase 1b). Encoded here so
// the batch contents are host-tested before any hardware run.
constexpr uint32_t BdwMiNoop=0;
constexpr uint32_t BdwMiBatchBufferEnd=0x05000000u;
// Gen8 MI_STORE_DATA_IMM, 64-bit address form, GGTT address space (bit 22):
// opcode 0x20, dword length 2 (four dwords total).
constexpr uint32_t BdwMiStoreDataImmGgtt=(0x20u<<23)|(1u<<22)|2u;
// Writes `value` to the 4-byte-aligned GGTT address and ends the batch.
// Returns the number of dwords written, or 0 when the arguments are invalid.
inline unsigned BdwBuildVcsSelftestBatch(uint32_t* out,unsigned capacity,uint64_t ggttAddress,uint32_t value){
    if(!out || capacity<6 || (ggttAddress&3) || ggttAddress>=(uint64_t{1}<<32) || !value)return 0;
    out[0]=BdwMiNoop;
    out[1]=BdwMiStoreDataImmGgtt;
    out[2]=uint32_t(ggttAddress);
    out[3]=uint32_t(ggttAddress>>32);
    out[4]=value;
    out[5]=BdwMiBatchBufferEnd;
    return 6;
}

// GT power-management and VCS register snapshot taken inside the forcewake
// critical section immediately before and after each VCS operation (reset,
// self-test). Investigates the render-clock sag seen on gen 390 after VCS use.
// All registers lie in pages the critical section already audits:
// BAR0+0xa000 (RPS/RC), BAR0+0x138000 (RC6 residency) and BAR0+0x12000 (VCS).
constexpr uint32_t BdwGtPmRegs[]={
    0xa008, // GEN6_RPNSWREQ: software frequency request
    0xa00c, // GEN6_RC_VIDEO_FREQ
    0xa010, // GEN6_RP_DOWN_TIMEOUT
    0xa014, // GEN6_RP_INTERRUPT_LIMITS
    0xa01c, // GEN6_RPSTAT1 (CAGF = current actual frequency)
    0xa024, // GEN6_RP_CONTROL
    0xa02c, // GEN6_RP_UP_THRESHOLD
    0xa030, // GEN6_RP_DOWN_THRESHOLD
    0xa068, // GEN6_RP_UP_EI
    0xa06c, // GEN6_RP_DOWN_EI
    0xa090, // GEN6_RC_CONTROL
    0xa094, // GEN6_RC_STATE
    0xa168, // GEN6_PMINTRMSK
    0x138108, // GEN6_GT_GFX_RC6 residency
    0x13810c, // GEN6_GT_GFX_RC6p residency
    0x1209c, // VCS MI_MODE
    0x1229c, // VCS GFX_MODE (execlist enable)
    0x1203c, // VCS RING_CTL
    0x12234, // VCS EXECLIST_STATUS
    0x12050, // VCS RING_PSMI_CTL (idle messaging)
    0x12080, // VCS HWS_PGA
    0x12098, // VCS HWSTAM
    0x12054, // VCS RING_MAX_IDLE (PWRCTX_MAXCNT: idle count before power-context save)
    0x120a8, // VCS RING_IMR
    0x120c0, // VCS INSTPM
    0x22054, // BCS RING_MAX_IDLE (comparison)
    0x2054,  // RCS RING_MAX_IDLE (comparison)
    0x138110, // GEN6_GT_GFX_RC6pp residency
    0x13805c, // GEN6_GT_THREAD_STATUS
};
constexpr unsigned BdwGtPmRegCount=sizeof(BdwGtPmRegs)/sizeof(BdwGtPmRegs[0]);
// Restored (exact previous value, write then verified readback) if a VCS
// operation changed them: plain GT power-management controls only. Status and
// counters (RPSTAT1, RC_STATE, residencies) and the masked VCS engine registers
// are reported, never written.
constexpr uint32_t BdwGtPmRestorable[]={0xa008,0xa00c,0xa010,0xa014,0xa024,0xa02c,0xa030,0xa068,0xa06c,0xa090,0xa168};
inline bool BdwGtPmRegRestorable(uint32_t reg){for(auto r:BdwGtPmRestorable)if(r==reg)return true;return false;}
struct BdwGtPmSnapshot {bool valid=false;uint32_t value[BdwGtPmRegCount]{};};
struct BdwGtPmRecord {
    BdwGtPmSnapshot first{};  // before the first VCS operation of this boot (firmware state)
    BdwGtPmSnapshot before{},after{};
    uint32_t changedMask=0;   // bit i: BdwGtPmRegs[i] differs after the operation
    uint32_t restoredMask=0;  // restored to the before value and verified
    uint32_t restoreFailedMask=0;
    uint8_t op=0;             // 1 reset, 2 self-test
};
// RPSTAT1 CAGF (bits 13:7 on Gen8) in MHz (50 MHz units).
inline uint32_t BdwGtCagfMhz(uint32_t rpstat1){return ((rpstat1>>7)&0x7f)*50;}
inline int BdwGtPmIndex(uint32_t reg){for(unsigned i=0;i<BdwGtPmRegCount;++i)if(BdwGtPmRegs[i]==reg)return int(i);return -1;}

// VCS power-management experiments (gen 391: a GEN6_GRDOM_MEDIA reset alone
// leaves the render clock at 400-430 MHz for the boot). Each is a debug
// subcommand run once, live, under the forcewake claim.
enum class BdwVcsPmOp : uint8_t {
    None=0,
    Sample,       // read-only snapshot (two, ~1 s apart, for residency deltas)
    StopStart,    // (a) MI_MODE STOP_RING set, wait for ring idle, clear (i915 legacy stop/init)
    IdleRing,     // (b) RING_CTL=0, RING_HEAD=0, RING_TAIL=0: a valid idle legacy ring
    RpsRewrite,   // (d) rewrite RP_* thresholds/EI/limits/control and PMINTRMSK with their current values
    EngineRestore,// (e) VCS MAX_IDLE, IMR, HWSTAM (plain) and PSMI_CTL, INSTPM (masked) back to first-boot values
};
inline const char* BdwVcsPmOpName(BdwVcsPmOp op){
    switch(op){
    case BdwVcsPmOp::Sample:return "sample";
    case BdwVcsPmOp::StopStart:return "stop-start";
    case BdwVcsPmOp::IdleRing:return "idle-ring";
    case BdwVcsPmOp::RpsRewrite:return "rps-rewrite";
    case BdwVcsPmOp::EngineRestore:return "engine-restore";
    default:return "none";
    }
}
// Registers EngineRestore may write, and whether they are masked (upper 16 bits
// select which lower bits a write changes).
struct BdwVcsRestoreReg {uint32_t reg;bool masked;};
constexpr BdwVcsRestoreReg BdwVcsEngineRestoreRegs[]={
    {0x12054,false},{0x120a8,false},{0x12098,false},{0x12050,true},{0x120c0,true}};
// RPS registers RpsRewrite rewrites with their current values (i915 gen8 RPS
// enable order: limits, timeouts, EI, thresholds, then control, then mask).
constexpr uint32_t BdwRpsRewriteRegs[]={0xa014,0xa010,0xa068,0xa06c,0xa02c,0xa030,0xa024,0xa168};
// Pure decision for the restore step; Io provides read(reg,v) and write(reg,v).
template<class Io> void BdwGtPmRestore(Io& io,BdwGtPmRecord& r){
    r.changedMask=r.restoredMask=r.restoreFailedMask=0;
    if(!r.before.valid || !r.after.valid)return;
    for(unsigned i=0;i<BdwGtPmRegCount;++i){
        if(r.before.value[i]==r.after.value[i])continue;
        r.changedMask|=1u<<i;
        const uint32_t reg=BdwGtPmRegs[i];
        if(!BdwGtPmRegRestorable(reg))continue;
        uint32_t back=0;
        if(io.write(reg,r.before.value[i]) && io.read(reg,back) && back==r.before.value[i])r.restoredMask|=1u<<i;
        else r.restoreFailedMask|=1u<<i;
    }
}
static_assert(BdwGtPmRegCount<=32,"mask width");

// Single-slot hand-off of one VCS device step from the gpu-video thread to the
// CPU0 application worker (serviced between RCS jobs and presents). The caller
// side never holds the diagnostic lease: it queues, waits and reads results;
// only the worker takes the lease, for the step itself.
class BdwVideoStepSlot {
    uint32_t state_=0; // 0 idle, 1 filling, 2 queued, 3 running, 4 done
public:
    enum : uint32_t {Idle=0,Filling=1,Queued=2,Running=3,Done=4};
    uint32_t state()const{return __atomic_load_n(&state_,__ATOMIC_ACQUIRE);}
    // Caller: reserve the slot to fill in the request; false if busy.
    bool begin(){uint32_t e=Idle;return __atomic_compare_exchange_n(&state_,&e,Filling,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
    void publish(){__atomic_store_n(&state_,Queued,__ATOMIC_RELEASE);}
    // Caller: take back a step the worker has not started (timeout). Nothing ran.
    bool withdraw(){uint32_t e=Queued;return __atomic_compare_exchange_n(&state_,&e,Idle,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
    // Caller: results read; free the slot.
    void finish(){__atomic_store_n(&state_,Idle,__ATOMIC_RELEASE);}
    // Worker: claim a queued step.
    bool claim(){uint32_t e=Queued;return __atomic_compare_exchange_n(&state_,&e,Running,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
    // Worker: could not take the diagnostic lease; leave it queued, untouched.
    void requeue(){__atomic_store_n(&state_,Queued,__ATOMIC_RELEASE);}
    void complete(){__atomic_store_n(&state_,Done,__ATOMIC_RELEASE);}
    bool pending()const{return state()==Queued;}
};
}

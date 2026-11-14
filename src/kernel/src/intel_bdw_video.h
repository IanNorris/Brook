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
    Quarantined,  // an operation failed in a way that leaves VCS state unknown
};

enum class BdwVideoRefusal : uint8_t {
    None, GateOff, Quarantined, Busy, ProbeFailed, NotQuiescent, ResetRefused, ResetUncertain, NotImplemented,
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
    }
    return "unknown";
}
inline const char* BdwVideoStageName(BdwVideoStage s){
    switch(s){
    case BdwVideoStage::Off:return "off";
    case BdwVideoStage::Enabled:return "enabled";
    case BdwVideoStage::Probed:return "probed";
    case BdwVideoStage::ResetOk:return "reset-ok";
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
    uint32_t probes_=0,resets_=0,refusals_=0;
    BdwVcsSample sample_{};
    void refuse(BdwVideoRefusal r){last_=r;++refusals_;}
public:
    BdwVideoStage stage()const{return stage_;}
    BdwVideoRefusal lastRefusal()const{return last_;}
    uint32_t probes()const{return probes_;}
    uint32_t resets()const{return resets_;}
    uint32_t refusals()const{return refusals_;}
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
    // Submission (self-test and execbuffer) is phase 1b and not implemented:
    // userspace is never told VCS exists.
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
}

#pragma once
#include <stdint.h>
namespace brook {
// Planner only: it performs no I/O and does not grant disk-write authority.
// Caller must independently validate source GPT copies, exact target capacity,
// canonical 128x128-byte entries and that every partition lies inside the
// payload, and pin the source and target. PMBR-last is not atomic publication.
// Sectors outside the selected payload and GPT metadata remain untouched.
struct SsdInstallGeometry {uint64_t diskSectors=0,payloadFirst=0,payloadSectors=0;};
inline bool SsdInstallGeometryValid(const SsdInstallGeometry& g) {
    return g.diskSectors>=68 && g.diskSectors<=(1ull<<48) && g.payloadFirst>=34 &&
        g.payloadFirst<g.diskSectors-33 && g.payloadSectors &&
        g.payloadSectors<=g.diskSectors-33-g.payloadFirst;
}
enum class SsdInstallAction : uint8_t { Zero,Copy,Verify,Flush,Done,Failed };
struct SsdInstallOperation {SsdInstallAction action;uint64_t lba;uint32_t sectors;};
enum class SsdInstallStage : uint8_t {
    InvalidateBackup,InvalidatePrimary,Payload,VerifyPayload,Backup,VerifyBackup,
    Primary,VerifyPrimary,Activate,VerifyActivation,Complete,Failed
};
class SsdInstallPlan {
    SsdInstallGeometry geometry_{};
    SsdInstallStage stage_=SsdInstallStage::Failed;
    uint64_t cursor_=0;
    bool flush_=false;
    static bool Writes(SsdInstallStage s) {
        return s==SsdInstallStage::InvalidateBackup || s==SsdInstallStage::InvalidatePrimary ||
            s==SsdInstallStage::Payload || s==SsdInstallStage::Backup ||
            s==SsdInstallStage::Primary || s==SsdInstallStage::Activate;
    }
    void Range(uint64_t& first,uint64_t& count) const {
        switch(stage_) {
        case SsdInstallStage::InvalidateBackup:first=geometry_.diskSectors-1;count=1;break;
        case SsdInstallStage::InvalidatePrimary:first=0;count=34;break;
        case SsdInstallStage::Payload:case SsdInstallStage::VerifyPayload:
            first=geometry_.payloadFirst;count=geometry_.payloadSectors;break;
        case SsdInstallStage::Backup:case SsdInstallStage::VerifyBackup:first=geometry_.diskSectors-33;count=33;break;
        case SsdInstallStage::Primary:case SsdInstallStage::VerifyPrimary:first=1;count=33;break;
        case SsdInstallStage::Activate:case SsdInstallStage::VerifyActivation:first=0;count=1;break;
        default:first=0;count=0;break;
        }
    }
public:
    explicit SsdInstallPlan(SsdInstallGeometry g):geometry_(g) {
        if(SsdInstallGeometryValid(g))stage_=SsdInstallStage::InvalidateBackup;
    }
    SsdInstallStage Stage() const {return stage_;}
    SsdInstallOperation Next() const {
        if(stage_==SsdInstallStage::Complete)return {SsdInstallAction::Done,0,0};
        if(stage_==SsdInstallStage::Failed)return {SsdInstallAction::Failed,0,0};
        if(flush_)return {SsdInstallAction::Flush,0,0};
        uint64_t first=0,count=0;Range(first,count);const uint64_t left=count-cursor_;
        const auto action=stage_==SsdInstallStage::InvalidateBackup || stage_==SsdInstallStage::InvalidatePrimary ?
            SsdInstallAction::Zero : Writes(stage_)?SsdInstallAction::Copy:SsdInstallAction::Verify;
        return {action,first+cursor_,uint32_t(left>128?128:left)};
    }
    // success means the caller has completed precisely this operation. Verify
    // succeeds only after independent source/target byte comparison; Flush
    // succeeds only after the device cache confirms durable completion.
    bool Finish(SsdInstallOperation op,bool success) {
        const auto expected=Next();
        if(stage_==SsdInstallStage::Failed || stage_==SsdInstallStage::Complete)return false;
        if(!success || op.action!=expected.action || op.lba!=expected.lba || op.sectors!=expected.sectors) {
            stage_=SsdInstallStage::Failed;return false;
        }
        if(flush_){flush_=false;cursor_=0;stage_=static_cast<SsdInstallStage>(uint8_t(stage_)+1);return true;}
        uint64_t first=0,count=0;Range(first,count);cursor_+=op.sectors;
        if(cursor_==count) {
            if(Writes(stage_))flush_=true;
            else {cursor_=0;stage_=static_cast<SsdInstallStage>(uint8_t(stage_)+1);}
        }
        return true;
    }
};
}

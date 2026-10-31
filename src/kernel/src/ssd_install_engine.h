#pragma once
#include "ssd_install_plan.h"
#include <string.h>

namespace brook {
enum class SsdInstallStepResult : uint8_t { Progress,Complete,Failed };
// Executes exactly one bounded operation per call. This engine has no device
// lookup, authorization or userspace entry point. The caller must complete
// preflight, pin an immutable source and exclusive target, and provide two
// distinct 65536-byte kernel-owned, physically non-aliasing buffers for the
// entire session. Calls must be serialized by that session's owner.
class SsdInstallEngine {
    SsdInstallPlan plan_;
    bool admitted_;
public:
    SsdInstallEngine(SsdInstallGeometry geometry,bool preflightPassed):plan_(geometry),admitted_(preflightPassed) {}
    SsdInstallStage Stage() const {return admitted_?plan_.Stage():SsdInstallStage::Failed;}
    template<class Io> SsdInstallStepResult Step(Io& io,uint8_t* source,uint8_t* target) {
        if(!admitted_)return SsdInstallStepResult::Failed;
        const auto op=plan_.Next();
        if(op.action==SsdInstallAction::Done)return SsdInstallStepResult::Complete;
        if(op.action==SsdInstallAction::Failed)return SsdInstallStepResult::Failed;
        bool ok=false;const uint32_t bytes=op.sectors*512;
        if(op.action==SsdInstallAction::Flush)ok=io.flushTarget()==0;
        else if(source && target && bytes && bytes<=65536 &&
                (uintptr_t(source)<uintptr_t(target) ? uintptr_t(target)-uintptr_t(source)>=65536 :
                                                     uintptr_t(source)-uintptr_t(target)>=65536)) {
            if(op.action==SsdInstallAction::Zero) {
                memset(source,0,bytes);ok=io.writeTarget(op.lba,source,bytes)==int(bytes);
            } else if(op.action==SsdInstallAction::Copy) {
                ok=io.readSource(op.lba,source,bytes)==int(bytes) && io.writeTarget(op.lba,source,bytes)==int(bytes);
            } else if(op.action==SsdInstallAction::Verify) {
                ok=io.readSource(op.lba,source,bytes)==int(bytes) && io.readTarget(op.lba,target,bytes)==int(bytes) &&
                    !memcmp(source,target,bytes);
            } else if(op.action==SsdInstallAction::VerifyZero) {
                ok=io.readTarget(op.lba,target,bytes)==int(bytes);
                if(ok)for(uint32_t i=0;i<bytes;++i)if(target[i]){ok=false;break;}
            }
        }
        if(!plan_.Finish(op,ok))return SsdInstallStepResult::Failed;
        return plan_.Next().action==SsdInstallAction::Done ? SsdInstallStepResult::Complete : SsdInstallStepResult::Progress;
    }
};
}

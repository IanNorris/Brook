#pragma once
#include <stdint.h>
#include "intel_bdw_oneshot.h"
#include "intel_bdw_submission.h"

namespace brook {
// VCS0 user job (VA-API phase 1c, docs/bdw-video-engine.md).
//
// One I915_EXEC_BSD execbuffer per instance lifetime: the same private page
// set, GGTT lease, takeover bind, execlist init, ELSP and CSB flow as the
// phase 1b self-test, but the context's PML4 is the root of a separate
// BdwExecTables instance built from the job's exec snapshot, and the ring
// starts the user's batch at its PPGTT address. The context image is fresh
// and restore-inhibited for every job (i965 re-emits all media state per
// batch), so nothing about one job's engine state survives into the next.
//
// The caller (the CPU0 application worker) owns the snapshot, its tables and
// this object; it must reset VCS before and after the run (the copy work
// item does both), retire the lease only after an acknowledged reset, and
// release the tables only after that verified stop.
class BdwVcsUserOneShot : public BdwBcsOneShot {
    uint64_t root_=0,batch_=0;
public:
    uint64_t Root()const{return root_;}
    uint64_t Batch()const{return batch_;}
    template<class Io> bool PrepareUser(Io& io,uint32_t firstIndex,uint64_t token,uint32_t nonce,uint64_t rootDma,
            uint64_t batchPpgtt,uint32_t (&staging)[2048]) {
        if(!rootDma || (rootDma&4095) || rootDma>=(uint64_t{1}<<39) || (batchPpgtt&7) || batchPpgtt>=(uint64_t{1}<<48))return false;
        if(!Allocate(io,firstIndex,token,nonce,Count))return false;
        for(unsigned i=0;i<allocated_;++i)if(pages_[i].dma==rootDma)return PrepareFailed(io);
        BdwBcsLaunch launch=Launch();
        launch.context.pml4Dma=rootDma;launch.batchPpgtt=batchPpgtt;
        uint32_t ring[14]{};
        if(!BdwBuildVcsUser(launch,staging,ring,descriptor_))return PrepareFailed(io);
        video_=videoUser_=true;videoData_=0;root_=rootDma;batch_=batchPpgtt;
        StoreImages(staging,nullptr,0,ring);
        return true;
    }
    // The private scratch hierarchy the job's tables point unmapped ranges at.
    bool ScratchTables(BdwPpgttPage (&out)[4])const{
        if(state_!=BdwOneShotState::Prepared || !videoUser_)return false;
        out[0]={pages_[ScratchData].dma,pages_[ScratchData].cpu};out[1]={pages_[ScratchPt].dma,pages_[ScratchPt].cpu};
        out[2]={pages_[ScratchPd].dma,pages_[ScratchPd].cpu};out[3]={pages_[ScratchPdpt].dma,pages_[ScratchPdpt].cpu};
        return true;
    }
    bool UserRecycle(){if(!Recycle())return false;root_=batch_=0;return true;}
};

// Admission and cache publication of a job's external table pages, supplied
// by the device owner under the same forcewake lease as the run.
struct BdwVcsTableSink {
    virtual bool admitExtraTables(BdwOwnedPage* pages,unsigned count)=0;
    virtual bool publishExtraTables(BdwOwnedPage* pages,unsigned count)=0;
protected:
    ~BdwVcsTableSink()=default;
};

// Worker-side classification of one run of a prepared VCS user job, from
// the forcewake adapter and copy-run results. Pure, so it is host-tested.
enum class BdwVcsUserClass : uint8_t {
    Completed,        // breadcrumb + CSB complete, engine reset, lease retired
    RefusedUntouched, // nothing reached VCS (claim busy, evidence, reset-before refused): retry or refuse
    FailedStopped,    // touched VCS and failed (hang, CSB fault), but reset acknowledged: fail fence, quarantine
    StoppedRetained,  // stopped, but the lease could not be unbound: fail fence, retain pages, quarantine
    Uncertain,        // GDRST, PAT or forcewake state unknown: panic
};
struct BdwVcsUserRunView {
    bool forcewakeRequested,forcewakeReleased,clearDelivered,workAttempted,workComplete;
    bool resetBeforeAttempted,resetBefore,resetUncertain,attempted,copyComplete,contextComplete;
    bool resetAfter,retired,patEstablished,patRestored;
    uint8_t refusalStep;uint32_t ggttWrites,portWrites;
};
inline BdwVcsUserClass BdwClassifyVcsUserRun(const BdwVcsUserRunView& v){
    if(v.forcewakeRequested && (!v.clearDelivered || !v.forcewakeReleased))return BdwVcsUserClass::Uncertain;
    if(v.resetUncertain || (v.attempted && !v.resetAfter) || (v.patEstablished && !v.patRestored))return BdwVcsUserClass::Uncertain;
    const bool touched=(v.resetBeforeAttempted && !(v.refusalStep==5 && !v.resetUncertain)) ||
        v.attempted || v.ggttWrites || v.portWrites;
    if(!touched)return BdwVcsUserClass::RefusedUntouched;
    if(!v.resetAfter)return BdwVcsUserClass::Uncertain; // touched (reset-before ran) but never stopped again
    if(!v.retired)return BdwVcsUserClass::StoppedRetained;
    if(v.copyComplete && v.contextComplete && v.workComplete)return BdwVcsUserClass::Completed;
    return BdwVcsUserClass::FailedStopped;
}
}

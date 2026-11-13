#include "intel_bdw_ioctl.h"
#include "profiler.h"
#include "intel_bdw_file.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_exec_snapshot.h"
#include "intel_bdw_submission_timing.h"
#include "intel_bdw_fence.h"
#include "memory/heap.h"
namespace brook {
namespace {
struct Version {int32_t major,minor,patch,pad;uint64_t nameLen,name,dateLen,date,descLen,desc;};
struct Capability {uint64_t capability,value;};
// The x86-64 i915 UAPI embeds a userspace pointer after the 32-bit selector.
struct GetParam {int32_t param,pad;uint64_t value;};
struct GemClose {uint32_t handle,pad;};
struct GemCreate {uint64_t size;uint32_t handle,pad;};
struct GemWait {uint32_t handle,flags;int64_t timeout;};
struct GemBusy {uint32_t handle,busy;};
struct GemMadvise {uint32_t handle,advice,retained;};
struct GemSetDomain {uint32_t handle,read,write;};
struct ContextCreate {uint32_t id,flags;};
struct ContextCreateExt {uint32_t id,flags;uint64_t extensions;};
struct ContextParam {uint32_t id,size;uint64_t param,value;};
struct ResetStats {uint32_t id,flags,resetCount,batchActive,batchPending,pad;};
struct SyncObject {uint32_t handle,flags;};
struct SyncArray {uint64_t handles;uint32_t count,pad;};
struct SyncWait {uint64_t handles;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
struct SyncTimelineWait {uint64_t handles,points;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
struct SyncTimelineArray {uint64_t handles,points;uint32_t count,flags;};
struct SyncTransfer {uint32_t source,destination;uint64_t sourcePoint,destinationPoint;uint32_t flags,pad;};
struct MmapOffset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct ExecBuffer {uint64_t objects;uint32_t count,start,length,dr1,dr4,clipCount;uint64_t clips,flags,context,fences;};
struct ExecObject {uint32_t handle,relocCount;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct ExecFence {uint32_t handle,flags;};
struct TimelineExtension {uint64_t next;uint32_t name,flags,reserved[4];uint64_t count,handles,points;};
static KMutex timelineAdmission{};
struct FencePlan {
    BdwGpuSync* objects[BdwMaxSubmissionFences]{};BdwFence* signals[BdwMaxSubmissionFences]{};bool timeline[BdwMaxSubmissionFences]{};uint32_t count=0;
    BdwFence* previous(BdwGpuSync* object)const{for(uint32_t i=count;i>0;--i)if(objects[i-1]==object && signals[i-1])return signals[i-1];return nullptr;}
    void release(){for(uint32_t i=0;i<count;++i){BdwFenceUnref(signals[i]);BdwGpuSyncUnref(objects[i]);}count=0;}
    void publish(){for(uint32_t i=0;i<count;++i)if(signals[i] &&
        !(timeline[i]?BdwGpuSyncPublishTimeline(objects[i],signals[i]):BdwGpuSyncReplace(objects[i],signals[i])))__builtin_trap();}
};
static_assert(sizeof(Version)==64 && sizeof(GetParam)==16 && sizeof(GemCreate)==16 && sizeof(GemWait)==16 && sizeof(MmapOffset)==32,"i915 ABI layout");
static_assert(sizeof(ExecBuffer)==64 && sizeof(ExecObject)==56,"i915 exec ABI layout");
}
bool BdwIntelSyncReplaceAdmitted(BdwGpuSync* sync,BdwFence* fence){
    KMutexLock(&timelineAdmission);const bool ok=BdwGpuSyncReplace(sync,fence);KMutexUnlock(&timelineAdmission);return ok;
}
bool BdwIntelSyncPointAdmitted(BdwGpuSync* sync,uint64_t value,BdwFence* fence){
    if(!sync || !value || !fence)return false;
    KMutexLock(&timelineAdmission);
    auto* point=BdwGpuSyncPrepareTimeline(sync,value,fence);
    const bool ok=point && BdwFenceComplete(point,true) && BdwGpuSyncPublishTimeline(sync,point);
    BdwFenceUnref(point);KMutexUnlock(&timelineAdmission);return ok;
}
static int64_t CopyExecSnapshot(BdwGpuFile* file,uint64_t arg,const BdwI915UserAccess& u,BdwExecSnapshot** out,FencePlan* plan){
    constexpr int64_t badfd=-9,fault=-14,invalid=-22,nomem=-12;
    if(!out)return invalid;
    *out=nullptr;
    if(!file)return badfd;
    if(!u.read)return fault;
    ExecBuffer request{};if(!u.read(u.context,&request,arg,sizeof(request)))return fault;
    constexpr uint64_t noReloc=uint64_t{1}<<11,handleLut=uint64_t{1}<<12,batchFirst=uint64_t{1}<<18,fenceArray=uint64_t{1}<<19,useExtensions=uint64_t{1}<<21;
    const uint64_t engine=request.flags&63;
    if(!request.count || request.count>8192 || !request.objects ||
       (request.flags&~(noReloc|handleLut|batchFirst|fenceArray|useExtensions|uint64_t{63})) || !(request.flags&noReloc) || engine>1 ||
       request.dr1 || request.dr4 || request.context>0xffffffffull || request.fences ||
       ((request.flags&useExtensions)?(!plan || (request.flags&fenceArray) || request.clipCount || !request.clips):
        (request.flags&fenceArray)?(!plan || !request.clipCount || request.clipCount>BdwMaxSubmissionFences || !request.clips):
                                  (request.clipCount || request.clips)) ||
       (request.start&7) || (request.length&7))return invalid;
    const uint64_t bytes=uint64_t(request.count)*sizeof(ExecObject);
    if(request.objects>~uint64_t{0}-(bytes-1))return fault;
    auto* raw=static_cast<ExecObject*>(kmalloc(bytes));
    if(!raw)return nomem;
    auto* objects=static_cast<BdwExecObjectInput*>(kmalloc(uint64_t(request.count)*sizeof(BdwExecObjectInput)));
    if(!objects){kfree(raw);return nomem;}
    int64_t result=0;
    if(!u.read(u.context,raw,request.objects,bytes))result=fault;
    else {
        for(uint32_t i=0;i<request.count;++i){const auto& obj=raw[i];
            if(obj.relocCount || obj.relocs || obj.reserved1 || obj.reserved2){result=invalid;break;}
            objects[i]={obj.handle,obj.offset,obj.alignment,obj.flags};
        }
        if(!result){
            const uint32_t batchIndex=(request.flags&batchFirst)?0:request.count-1;
            *out=BdwExecSnapshotCreate(file,objects,request.count,batchIndex,request.start,request.length);
            if(!*out)result=invalid;
            else {
                BdwExecSnapshotSetOrigin(*out,u.submitterPid,uint32_t(request.context),u.submitterIncarnation);
            }
            if(!result && (request.context || u.renderExperiment)){
                auto* context=request.context?BdwGpuFileGetContext(file,uint32_t(request.context)):
                    BdwGpuFileGetDefaultContext(file);
                if(!context)result=-2;
                else {if(!BdwExecSnapshotBindContext(*out,context))result=invalid;BdwGpuContextUnref(context);}
                if(result){BdwExecSnapshotRelease(*out,false);*out=nullptr;}
            }
        }
    }
    if(!result && (request.flags&(fenceArray|useExtensions))){
        ExecFence fences[BdwMaxSubmissionFences]{};uint64_t points[BdwMaxSubmissionFences]{};uint32_t count=request.clipCount;uint64_t handles=request.clips,values=0;
        if(request.flags&useExtensions){
            TimelineExtension ext{};
            if(!u.read(u.context,&ext,request.clips,sizeof(ext)))result=fault;
            else if(ext.next || ext.name || ext.flags || ext.reserved[0] || ext.reserved[1] || ext.reserved[2] || ext.reserved[3] ||
                    !ext.count || ext.count>BdwMaxSubmissionFences || !ext.handles || !ext.points)result=invalid;
            else {count=uint32_t(ext.count);handles=ext.handles;values=ext.points;}
        }
        const uint64_t fenceBytes=uint64_t(count)*sizeof(ExecFence),pointBytes=uint64_t(count)*sizeof(uint64_t);
        if(!result && (handles>~uint64_t{0}-(fenceBytes-1) || !u.read(u.context,fences,handles,fenceBytes)))result=fault;
        if(!result && values && (values>~uint64_t{0}-(pointBytes-1) || !u.read(u.context,points,values,pointBytes)))result=fault;
        if(!result)for(uint32_t i=0;i<count && !result;++i){
            const auto entry=fences[i];
            if(!entry.handle || !entry.flags || (entry.flags&~3u)){result=invalid;break;}
            auto* object=BdwGpuFileGetSync(file,entry.handle);if(!object){result=-2;break;}
            auto* previous=plan->previous(object);
            const uint32_t index=plan->count++;plan->objects[index]=object;plan->timeline[index]=points[i]!=0;
            if(entry.flags&1){auto* wait=points[i]?BdwGpuSyncFindTimeline(object,points[i]):BdwGpuSyncAcquire(object);
                if(!wait)result=invalid;
                else {if(!BdwExecSnapshotAddWaitFence(*out,wait))result=invalid;BdwFenceUnref(wait);}}
            if(!result && (entry.flags&2)){auto* signal=points[i]?(previous?BdwFencePrepareTimelineAfter(previous,points[i]):BdwGpuSyncPrepareTimeline(object,points[i])):BdwFenceCreate();
                if(!signal)result=nomem;
                else {plan->signals[index]=signal;if(!BdwExecSnapshotAddSignalFence(*out,signal))result=invalid;}}
        }
        if(result){BdwExecSnapshotRelease(*out,false);*out=nullptr;plan->release();}
    }
    kfree(objects);kfree(raw);return result;
}
int64_t BdwIntelCopyExecSnapshot(BdwGpuFile* file,uint64_t arg,const BdwI915UserAccess& u,BdwExecSnapshot** out){
    return CopyExecSnapshot(file,arg,u,out,nullptr);
}
int64_t BdwIntelRenderIoctl(BdwGpuFile* file,uint64_t cmd,uint64_t arg,const BdwI915UserAccess& u){
    constexpr int64_t badfd=-9,notty=-25,fault=-14,invalid=-22,noent=-2,nomem=-12;
    if(!file)return badfd;
    if(!u.read || !u.writable || !u.write)return fault;
    cmd&=0xffffffffu;if(((cmd>>8)&255)!='d')return notty;
    const uint32_t nr=uint32_t(cmd&255),size=uint32_t((cmd>>16)&0x3fff);
    if(nr==0 && size==sizeof(Version)){
        Version v{};if(!u.read(u.context,&v,arg,sizeof(v)) || !u.writable(u.context,arg,sizeof(v)))return fault;
        v.major=1;v.minor=6;v.patch=0;
        auto string=[&](uint64_t address,uint64_t& length,const char* text){
            uint64_t actual=0;while(text[actual])++actual;
            const uint64_t n=length<actual?length:actual;length=actual;
            return !address || !n || u.write(u.context,address,text,n);
        };
        if(!string(v.name,v.nameLen,"i915") || !string(v.date,v.dateLen,"20080730") ||
           !string(v.desc,v.descLen,"Brook Intel render"))return fault;
        return u.write(u.context,arg,&v,sizeof(v))?0:fault;
    }
    if(nr==0x0c && size==sizeof(Capability)){
        Capability c{};if(!u.read(u.context,&c,arg,sizeof(c)) || !u.writable(u.context,arg,sizeof(c)))return fault;
        // The experimental node supports binary syncobj creation, waits,
        // signaling and descriptor transfer. Timeline support is a separate
        // capability and remains unadvertised. PRIME is Intel-to-Intel only.
        c.value=0;
        if(u.renderExperiment && u.submit){
            if(c.capability==5)c.value=3;
            else if(c.capability==0x13 && u.nowNs && u.sleepNs)c.value=1;
        }
        return u.write(u.context,arg,&c,sizeof(c))?0:fault;
    }
    if(nr==0x46 && size==sizeof(GetParam)){
        GetParam p{};if(!u.read(u.context,&p,arg,sizeof(p)))return fault;
        int32_t value=0;
        switch(p.param){
        // Publication is restricted to the matching live Dell PCI function.
        case 4: value=0x1616;break; // CHIPSET_ID
        case 32: value=9;break;     // REVISION
        case 19: value=(u.nowNs && u.sleepNs)?1:0;break; // HAS_WAIT_TIMEOUT
        // Iris can use the implemented WB mmap-offset path in this explicit
        // bring-up mode. Other cache modes still fail the mmap-offset ioctl.
        case 40: value=(u.renderExperiment && u.submit)?4:0;break;
        case 2:  // ALLOW_BATCHBUFFER
        case 9:  // HAS_EXECBUF2
        case 24: // HAS_PINNED_BATCHES
        case 25: // HAS_EXEC_NO_RELOC
        case 26: // HAS_EXEC_HANDLE_LUT
        case 37: // HAS_EXEC_SOFTPIN
        case 43: // HAS_EXEC_ASYNC
        case 48: // HAS_EXEC_BATCH_FIRST
            value=(u.renderExperiment && u.submit)?1:0;break;
        // Bring-up assertions for Iris/HasVK, scoped to the explicitly opted-in
        // Dell experiment. Fence arrays remain bounded and full 3D context
        // isolation is not yet validated; do not enable outside this mode.
        case 49: // HAS_EXEC_FENCE_ARRAY
        case 50: // HAS_CONTEXT_ISOLATION (render engine class bit)
        // This advertises the execbuffer timeline-fence extension, not the
        // separate DRM_CAP_SYNCOBJ_TIMELINE interface (which remains disabled).
        case 55: // HAS_EXEC_TIMELINE_FENCES
            value=(u.renderExperiment && u.submit)?1:0;break;
        case 41: // HAS_SCHEDULER
        case 44: // HAS_EXEC_FENCE
        case 45: // HAS_EXEC_CAPTURE
        case 53: // HAS_EXEC_SUBMIT_FENCE
            break;
        default:return invalid;
        }
        return u.write(u.context,p.value,&value,sizeof(value))?0:fault;
    }
    if(nr==0x09 && size==sizeof(GemClose)){
        GemClose c{};if(!u.read(u.context,&c,arg,sizeof(c)))return fault;
        if(c.pad)return invalid;
        return BdwGpuFileCloseBuffer(file,c.handle)?0:noent;
    }
    if(nr==0x5f && size==sizeof(GemSetDomain)){
        GemSetDomain d{};if(!u.read(u.context,&d,arg,sizeof(d)))return fault;
        if(d.read!=1 || (d.write!=0 && d.write!=1))return invalid;
        auto* buffer=BdwGpuFileGetBuffer(file,d.handle);if(!buffer)return noent;
        if(!u.sleepNs){BdwGpuBufferUnref(buffer);return notty;}
        int64_t result=0;
        for(;;){const auto attempt=BdwGpuBufferTryCpuTransition(buffer);
            if(attempt==BdwCpuTransitionResult::Ready)break;
            if(attempt==BdwCpuTransitionResult::Failed){result=-5;break;}
            if(!u.sleepNs(u.context,1000000)){result=-4;break;}
        }
        BdwGpuBufferUnref(buffer);return result;
    }
#ifdef BROOK_GPU_BLOCKED_RECOVERY_TEST
    // Explicit test-kernel-only injection; ordinary kernels do not implement
    // this ioctl. It creates a pending dependency without issuing GPU work.
    if(nr==0xee && size==sizeof(SyncObject)){
        SyncObject s{};if(!u.renderExperiment || !u.read(u.context,&s,arg,sizeof(s)))return fault;
        if(s.flags!=0x42524b54u && s.flags!=0x42524b55u)return invalid;
        auto* sync=BdwGpuFileGetSync(file,s.handle);if(!sync)return noent;
        if(s.flags==0x42524b55u){
            auto* pending=BdwGpuSyncAcquire(sync);const bool ok=pending && BdwFenceComplete(pending,true);
            BdwFenceUnref(pending);BdwGpuSyncUnref(sync);return ok?0:-22;
        }
        auto* pending=BdwFenceCreate();if(!pending){BdwGpuSyncUnref(sync);return nomem;}
        const bool ok=BdwIntelSyncReplaceAdmitted(sync,pending);BdwFenceUnref(pending);BdwGpuSyncUnref(sync);
        return ok?0:-5;
    }
#endif
    if(nr==0xbf && size==sizeof(SyncObject)){
        SyncObject s{};if(!u.read(u.context,&s,arg,sizeof(s)) || !u.writable(u.context,arg,sizeof(s)))return fault;
        if(s.flags&~1u)return invalid;
        s.handle=BdwGpuFileCreateSync(file,(s.flags&1)!=0);if(!s.handle)return nomem;
        if(!u.write(u.context,arg,&s,sizeof(s))){BdwGpuFileDestroySync(file,s.handle);return fault;}
        return 0;
    }
    if(nr==0xc0 && size==sizeof(SyncObject)){
        SyncObject s{};if(!u.read(u.context,&s,arg,sizeof(s)))return fault;
        if(s.flags)return invalid;
        return BdwGpuFileDestroySync(file,s.handle)?0:noent;
    }
    if((nr==0xc4 || nr==0xc5) && size==sizeof(SyncArray)){
        SyncArray request{};if(!u.read(u.context,&request,arg,sizeof(request)))return fault;
        if(request.pad || !request.count || request.count>BdwMaxSubmissionFences || !request.handles ||
           request.handles>~uint64_t{0}-(uint64_t(request.count)*4-1))return invalid;
        uint32_t handles[BdwMaxSubmissionFences]{};BdwGpuSync* objects[BdwMaxSubmissionFences]{};
        if(!u.read(u.context,handles,request.handles,uint64_t(request.count)*4))return fault;
        uint32_t acquired=0;for(;acquired<request.count;++acquired)
            if(!(objects[acquired]=BdwGpuFileGetSync(file,handles[acquired])))break;
        if(acquired!=request.count){for(uint32_t i=0;i<acquired;++i)BdwGpuSyncUnref(objects[i]);return noent;}
        BdwFence* signaled=nr==0xc5?BdwFenceCreate(true):nullptr;
        if(nr==0xc5 && !signaled){for(uint32_t i=0;i<acquired;++i)BdwGpuSyncUnref(objects[i]);return nomem;}
        struct AdmissionGuard {AdmissionGuard(){KMutexLock(&timelineAdmission);}
            ~AdmissionGuard(){KMutexUnlock(&timelineAdmission);}} admission;
        for(uint32_t i=0;i<acquired;++i){if(!BdwGpuSyncReplace(objects[i],signaled))__builtin_trap();BdwGpuSyncUnref(objects[i]);}
        BdwFenceUnref(signaled);return 0;
    }
    // WAIT_DEADLINE is an optional scheduling hint; this polling backend has
    // no deadline propagation, while the absolute timeout remains enforced.
    if(nr==0xc3 && size==sizeof(SyncWait)){
        SyncWait request{};if(!u.read(u.context,&request,arg,sizeof(request)) || !u.writable(u.context,arg,sizeof(request)))return fault;
        if(request.pad || (request.flags&~11u))return invalid;
        if(!request.count)return 0;
        if(request.count>BdwMaxSubmissionFences || !request.handles ||
           request.handles>~uint64_t{0}-(uint64_t(request.count)*4-1))return invalid;
        if(!u.nowNs || !u.sleepNs)return notty;
        uint32_t handles[BdwMaxSubmissionFences]{};BdwGpuSync* objects[BdwMaxSubmissionFences]{};BdwFence* fences[BdwMaxSubmissionFences]{};
        if(!u.read(u.context,handles,request.handles,uint64_t(request.count)*4))return fault;
        uint32_t acquired=0;for(;acquired<request.count;++acquired)
            if(!(objects[acquired]=BdwGpuFileGetSync(file,handles[acquired])))break;
        int64_t result=0;
        if(acquired!=request.count)result=noent;
        else for(;;){
            bool all=true,any=false,missing=false;uint32_t first=0;
            for(uint32_t i=0;i<request.count;++i){
                if(!fences[i])fences[i]=BdwGpuSyncAcquire(objects[i]);
                if(!fences[i]){missing=true;all=false;continue;}
                if((request.flags&4) || BdwFenceState(fences[i])){if(!any)first=i;any=true;}else all=false;
            }
            if(missing && !(request.flags&6)){result=invalid;break;}
            if((request.flags&1)?all:any){request.first=first;break;}
            const uint64_t now=u.nowNs(u.context);
            if(request.timeout<=0 || now>=uint64_t(request.timeout)){result=-62;break;}
            const uint64_t remaining=uint64_t(request.timeout)-now;
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
            if (ProfilerFrameTraceActive())
                for(uint32_t i=0;i<request.count;++i)
                    ProfilerTraceGpu(static_cast<uint16_t>(u.submitterPid),FrameFenceWait,
                                     fences[i]?fences[i]->traceId:0,handles[i]);
#endif
            if(!u.sleepNs(u.context,remaining<1000000?remaining:1000000)){result=-4;break;}
        }
        for(uint32_t i=0;i<acquired;++i){BdwFenceUnref(fences[i]);BdwGpuSyncUnref(objects[i]);}
        return u.write(u.context,arg,&request,sizeof(request))?result:fault;
    }
    if(nr==0xca && size==sizeof(SyncTimelineWait)){
        SyncTimelineWait request{};
        if(!u.read(u.context,&request,arg,sizeof(request)) || !u.writable(u.context,arg,sizeof(request)))return fault;
        if(request.pad || (request.flags&~15u))return invalid;
        if(!request.count)return 0;
        if(request.count>BdwMaxSubmissionFences ||
           !request.handles || request.handles>~uint64_t{0}-(uint64_t(request.count)*4-1) ||
           (request.points && request.points>~uint64_t{0}-(uint64_t(request.count)*8-1)))return invalid;
        if(!u.nowNs || !u.sleepNs)return notty;
        uint32_t handles[BdwMaxSubmissionFences]{};uint64_t points[BdwMaxSubmissionFences]{};BdwGpuSync* objects[BdwMaxSubmissionFences]{};BdwFence* fences[BdwMaxSubmissionFences]{};
        if(!u.read(u.context,handles,request.handles,uint64_t(request.count)*4) ||
           (request.points && !u.read(u.context,points,request.points,uint64_t(request.count)*8)))return fault;
        uint32_t acquired=0;for(;acquired<request.count;++acquired)
            if(!(objects[acquired]=BdwGpuFileGetSync(file,handles[acquired])))break;
        int64_t result=0;
        if(acquired!=request.count)result=noent;
        else for(;;){
            bool all=true,any=false,missing=false;uint32_t first=0;
            for(uint32_t i=0;i<request.count;++i){
                if(!fences[i])fences[i]=points[i]?BdwGpuSyncFindTimeline(objects[i],points[i]):BdwGpuSyncAcquire(objects[i]);
                if(!fences[i]){missing=true;all=false;continue;}
                if((request.flags&4) || BdwFenceState(fences[i])){if(!any)first=i;any=true;}else all=false;
            }
            if(missing && !(request.flags&6)){result=invalid;break;}
            if((request.flags&1)?all:any){request.first=first;break;}
            const uint64_t now=u.nowNs(u.context);
            if(request.timeout<=0 || now>=uint64_t(request.timeout)){result=-62;break;}
            const uint64_t remaining=uint64_t(request.timeout)-now;
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
            if (ProfilerFrameTraceActive())
                for(uint32_t i=0;i<request.count;++i)
                    ProfilerTraceGpu(static_cast<uint16_t>(u.submitterPid),FrameFenceWait,
                                     fences[i]?fences[i]->traceId:0,handles[i]);
#endif
            if(!u.sleepNs(u.context,remaining<1000000?remaining:1000000)){result=-4;break;}
        }
        for(uint32_t i=0;i<acquired;++i){BdwFenceUnref(fences[i]);BdwGpuSyncUnref(objects[i]);}
        return u.write(u.context,arg,&request,sizeof(request))?result:fault;
    }
    // Take an immutable source reference before updating the destination, and never hold
    // admission locks while waiting for another thread to submit a fence.
    if(nr==0xcc && size==sizeof(SyncTransfer)){
        SyncTransfer request{};if(!u.read(u.context,&request,arg,sizeof(request)))return fault;
        if(request.pad || (request.flags&~2u))return invalid;
        auto* source=BdwGpuFileGetSync(file,request.source);if(!source)return noent;
        auto* destination=BdwGpuFileGetSync(file,request.destination);
        if(!destination){BdwGpuSyncUnref(source);return noent;}
        int64_t result=0;BdwFence* fence=nullptr;
        if((request.flags&2) && (!u.nowNs || !u.sleepNs))result=notty;
        uint64_t deadline=0;
        if(!result && (request.flags&2)){
            const uint64_t now=u.nowNs(u.context);deadline=now>~uint64_t{0}-5000000000ull?~uint64_t{0}:now+5000000000ull;
        }
        while(!result && !fence){
            fence=request.sourcePoint?BdwGpuSyncFindTimeline(source,request.sourcePoint):BdwGpuSyncAcquire(source);
            if(fence)break;
            if(!(request.flags&2)){result=invalid;break;}
            const uint64_t now=u.nowNs(u.context);if(now>=deadline){result=-62;break;}
            const uint64_t remaining=deadline-now;
            if(!u.sleepNs(u.context,remaining<1000000?remaining:1000000)){result=-4;break;}
        }
        if(!result && !request.destinationPoint){if(!BdwIntelSyncReplaceAdmitted(destination,fence))result=invalid;}
        else if(!result && !BdwIntelSyncPointAdmitted(destination,request.destinationPoint,fence))result=invalid;
        BdwFenceUnref(fence);BdwGpuSyncUnref(source);BdwGpuSyncUnref(destination);return result;
    }
    if(nr==0xcb && size==sizeof(SyncTimelineArray)){
        SyncTimelineArray request{};if(!u.read(u.context,&request,arg,sizeof(request)))return fault;
        if((request.flags&~1u) || !request.count || request.count>BdwMaxSubmissionFences || !request.handles || !request.points ||
           request.handles>~uint64_t{0}-(uint64_t(request.count)*4-1) ||
           request.points>~uint64_t{0}-(uint64_t(request.count)*8-1))return invalid;
        if(!u.writable(u.context,request.points,uint64_t(request.count)*8))return fault;
        uint32_t handles[BdwMaxSubmissionFences]{};uint64_t points[BdwMaxSubmissionFences]{};BdwGpuSync* objects[BdwMaxSubmissionFences]{};
        if(!u.read(u.context,handles,request.handles,uint64_t(request.count)*4))return fault;
        uint32_t acquired=0;for(;acquired<request.count;++acquired)
            if(!(objects[acquired]=BdwGpuFileGetSync(file,handles[acquired])))break;
        if(acquired!=request.count){for(uint32_t i=0;i<acquired;++i)BdwGpuSyncUnref(objects[i]);return noent;}
        for(uint32_t i=0;i<request.count;++i){points[i]=BdwGpuSyncQueryTimeline(objects[i],(request.flags&1)!=0);BdwGpuSyncUnref(objects[i]);}
        return u.write(u.context,request.points,points,uint64_t(request.count)*8)?0:fault;
    }
    if(nr==0xcd && size==sizeof(SyncTimelineArray)){
        SyncTimelineArray request{};if(!u.read(u.context,&request,arg,sizeof(request)))return fault;
        if(request.flags || !request.count || request.count>BdwMaxSubmissionFences || !request.handles ||
           request.handles>~uint64_t{0}-(uint64_t(request.count)*4-1) ||
           (request.points && request.points>~uint64_t{0}-(uint64_t(request.count)*8-1)))return invalid;
        uint32_t handles[BdwMaxSubmissionFences]{};uint64_t points[BdwMaxSubmissionFences]{};
        if(!u.read(u.context,handles,request.handles,uint64_t(request.count)*4) ||
           (request.points && !u.read(u.context,points,request.points,uint64_t(request.count)*8)))return fault;
        struct AdmissionGuard {AdmissionGuard(){KMutexLock(&timelineAdmission);}
            ~AdmissionGuard(){KMutexUnlock(&timelineAdmission);}} admission;
        FencePlan plan{};int64_t result=0;
        for(uint32_t i=0;i<request.count && !result;++i){
            auto* object=BdwGpuFileGetSync(file,handles[i]);if(!object){result=noent;break;}
            auto* previous=plan.previous(object);
            const uint32_t index=plan.count++;plan.objects[index]=object;plan.timeline[index]=true;
            auto* signal=previous?BdwFencePrepareTimelineAfter(previous,points[i]):BdwGpuSyncPrepareTimeline(object,points[i]);
            if(!signal){result=invalid;break;}
            plan.signals[index]=signal;if(!BdwFenceComplete(signal,true)){result=invalid;break;}
        }
        if(!result)plan.publish();plan.release();return result;
    }
    if(nr==0x6d && (size==sizeof(ContextCreate) || size==sizeof(ContextCreateExt))){
        ContextCreateExt c{};
        if(!u.read(u.context,&c,arg,size) || !u.writable(u.context,arg,size))return fault;
        if(c.flags || c.extensions)return invalid;
        c.id=BdwGpuFileCreateContext(file);if(!c.id)return nomem;
        if(!u.write(u.context,arg,&c,size)){BdwGpuFileDestroyContext(file,c.id);return fault;}
        return 0;
    }
    if(nr==0x72 && size==sizeof(ResetStats)){
        ResetStats stats{};if(!u.read(u.context,&stats,arg,sizeof(stats)) || !u.writable(u.context,arg,sizeof(stats)))return fault;
        if(stats.flags || stats.pad)return invalid;
        auto* context=stats.id?BdwGpuFileGetContext(file,stats.id):BdwGpuFileGetDefaultContext(file);
        if(!context)return noent;
        const uint32_t loss=BdwGpuContextLoss(context);BdwGpuContextUnref(context);
        // The context is poisoned on the first lost batch. Report sticky
        // active/pending loss; the global counter is hidden as in an
        // unprivileged i915 query, rather than inventing reset history.
        stats.resetCount=0;stats.batchActive=(loss&1)?1:0;stats.batchPending=(loss&2)?1:0;
        return u.write(u.context,arg,&stats,sizeof(stats))?0:fault;
    }
    if(nr==0x6e && size==sizeof(ContextCreate)){
        ContextCreate c{};if(!u.read(u.context,&c,arg,sizeof(c)))return fault;
        if(c.flags || !c.id)return invalid;
        return BdwGpuFileDestroyContext(file,c.id)?0:noent;
    }
    if((nr==0x74 || nr==0x75) && size==sizeof(ContextParam)){
        ContextParam p{};if(!u.read(u.context,&p,arg,sizeof(p)))return fault;
        if(p.size)return invalid;
        if(p.id){auto* context=BdwGpuFileGetContext(file,p.id);if(!context)return noent;BdwGpuContextUnref(context);}
        if(nr==0x74){
            if(!u.writable(u.context,arg,sizeof(p)))return fault;
            switch(p.param){
            case 3:p.value=uint64_t{1}<<48;break; // GTT_SIZE, four-level PPGTT
            case 6:p.value=0;break; // default priority
            case 8:p.value=0;break; // failed contexts are not recovered
            default:return invalid;
            }
            return u.write(u.context,arg,&p,sizeof(p))?0:fault;
        }
        // The single worker has one priority and lost contexts stay lost.
        if((p.param==6 || p.param==8) && !p.value)return 0;
        return invalid;
    }
    if(nr==0x57 && size==sizeof(GemBusy)){
        GemBusy b{};if(!u.read(u.context,&b,arg,sizeof(b)) || !u.writable(u.context,arg,sizeof(b)))return fault;
        auto* buffer=BdwGpuFileGetBuffer(file,b.handle);if(!buffer)return noent;
        uint32_t uses=0;bool failed=false;BdwGpuBufferGpuStatus(buffer,uses,failed);BdwGpuBufferUnref(buffer);
        if(failed)return -5;
        // Only RCS is supported. The engine read/write fields are hints in
        // the i915 ABI; until access modes are tracked, conservatively mark
        // both while a queued or executing request retains the object.
        b.busy=uses?0x10001u:0;
        return u.write(u.context,arg,&b,sizeof(b))?0:fault;
    }
    if(nr==0x66 && size==sizeof(GemMadvise)){
        GemMadvise m{};if(!u.read(u.context,&m,arg,sizeof(m)) || !u.writable(u.context,arg,sizeof(m)))return fault;
        if(m.advice>1)return invalid;
        auto* buffer=BdwGpuFileGetBuffer(file,m.handle);if(!buffer)return noent;
        BdwGpuBufferUnref(buffer);
        // Brook does not purge GEM backing. Both WILLNEED and DONTNEED are
        // accepted as advice, and the current contents remain retained.
        m.retained=1;return u.write(u.context,arg,&m,sizeof(m))?0:fault;
    }
    if(nr==0x6c && size==sizeof(GemWait)){
        GemWait w{};if(!u.read(u.context,&w,arg,sizeof(w)) || !u.writable(u.context,arg,sizeof(w)))return fault;
        if(w.flags)return invalid;
        auto* buffer=BdwGpuFileGetBuffer(file,w.handle);if(!buffer)return noent;
        if(!u.nowNs || !u.sleepNs){BdwGpuBufferUnref(buffer);return notty;}
        const int64_t original=w.timeout;const uint64_t start=u.nowNs(u.context);
        const uint64_t deadline=original>0 && uint64_t(original)>~uint64_t{0}-start
            ? ~uint64_t{0} : original>0 ? start+uint64_t(original) : start;
        int64_t result=0;uint64_t now=start;
        for(;;){
            uint32_t uses=0;bool failed=false;BdwGpuBufferGpuStatus(buffer,uses,failed);
            if(failed){result=-5;break;} // EIO: failed/uncertain GPU use
            if(!uses)break;
            now=u.nowNs(u.context);if(now<start){result=-5;break;}
            if(original>=0 && (original==0 || now>=deadline)){result=-62;break;} // ETIME
            const uint64_t wait=original<0?1000000ull:(deadline-now<1000000ull?deadline-now:1000000ull);
            if(!u.sleepNs(u.context,wait)){result=-4;break;} // EINTR
        }
        now=u.nowNs(u.context);
        if(original>0)w.timeout=now<start?0:now>=deadline?0:static_cast<int64_t>(deadline-now);
        BdwGpuBufferUnref(buffer);
        return u.write(u.context,arg,&w,sizeof(w))?result:fault;
    }
    if(nr==0x5b && size==sizeof(GemCreate)){
        GemCreate c{};if(!u.read(u.context,&c,arg,sizeof(c)) || !u.writable(u.context,arg,sizeof(c)))return fault;
        if(c.pad || !c.size || c.size>(uint64_t{65536}<<12))return invalid;
        uint64_t rounded=0;const uint32_t handle=BdwGpuFileCreateBuffer(file,c.size,rounded);
        if(!handle)return nomem;
        c.handle=handle;c.size=rounded;
        if(!u.write(u.context,arg,&c,sizeof(c))){BdwGpuFileCloseBuffer(file,handle);return fault;}
        return 0;
    }
    if(nr==0x64 && size==sizeof(MmapOffset)){
        MmapOffset m{};if(!u.read(u.context,&m,arg,sizeof(m)) || !u.writable(u.context,arg,sizeof(m)))return fault;
        if(m.pad || m.extensions || m.flags!=2)return invalid;
        m.offset=BdwGpuFileMapToken(file,m.handle);if(!m.offset)return noent;
        return u.write(u.context,arg,&m,sizeof(m))?0:fault;
    }
    if(nr==0x69 && size==sizeof(ExecBuffer)){
        if(!u.submit)return notty;
        if(bool(u.beginExec)!=bool(u.endExec))return invalid;
        BdwSubmissionPreparation preparation{};
        if(u.beginExec){const auto started=u.beginExec(u.context,u.submitterIncarnation,preparation);if(started)return started;}
        struct PreparationGuard {const BdwI915UserAccess& u;BdwSubmissionPreparation& p;
            ~PreparationGuard(){if(u.endExec)u.endExec(u.context,p);}} preparationGuard{u,preparation};
        // Serialize preparation, admission and publication so another
        // submission cannot replace a prepared timeline predecessor.
 #if defined(BROOK_GPU_SUBMISSION_TIMING)
        const uint64_t admissionStarted=BdwTimingNow();
 #endif
        struct AdmissionGuard {AdmissionGuard(){KMutexLock(&timelineAdmission);}
            ~AdmissionGuard(){KMutexUnlock(&timelineAdmission);}} admission;
 #if defined(BROOK_GPU_SUBMISSION_TIMING)
        const uint64_t admissionAcquired=BdwTimingNow();
        BdwTimingOwnerElapsed(u.submitterIncarnation,BdwTimingAdmissionWait,admissionStarted,admissionAcquired);
 #endif
        BdwExecSnapshot* snapshot=nullptr;FencePlan plan{};
        const int64_t decoded=CopyExecSnapshot(file,arg,u,&snapshot,&plan);
 #if defined(BROOK_GPU_SUBMISSION_TIMING)
        BdwTimingOwnerElapsed(u.submitterIncarnation,BdwTimingSnapshotBuild,admissionAcquired,BdwTimingNow());
 #endif
        if(decoded)return decoded;
        int64_t submitted=0;
        const bool claimed=u.handoffSubmit && u.handoffSubmit(u.context,snapshot,&submitted);
        if(!claimed)submitted=u.submit(u.context,snapshot);
        if(!submitted){plan.publish();plan.release();return 0;}
        plan.release();
        // A failed callback is contractually unarmed and retains no owner.
        if(!BdwExecSnapshotRelease(snapshot,false))__builtin_trap();
        return submitted<0?submitted:-5;
    }
    return notty;
}
}

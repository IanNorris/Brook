#include "intel_bdw_refusal.h"
#include "intel_bdw_ioctl_trace.h"
#include "intel_bdw_video.h"
#if !defined(BROOK_HOST_TEST)
#include "process.h"
#endif
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
// VA-API phase 2a (libdrm_intel / i965) legacy GEM ioctls.
struct GemMmap {uint32_t handle,pad;uint64_t offset,size,addr,flags;};
struct GemMmapGtt {uint32_t handle,pad;uint64_t offset;};
struct GemTiling {uint32_t handle,mode,stride,swizzle;};       // SET_TILING
struct GemGetTiling {uint32_t handle,mode,swizzle,physSwizzle;}; // GET_TILING
struct GemAperture {uint64_t size,available;};
struct GemSwFinish {uint32_t handle;};
struct GemCaching {uint32_t handle,caching;};
struct GemRw {uint32_t handle,pad;uint64_t offset,size,data;};    // PREAD/PWRITE
static_assert(sizeof(GemMmap)==40 && sizeof(GemMmapGtt)==16 && sizeof(GemTiling)==16 && sizeof(GemGetTiling)==16 &&
              sizeof(GemAperture)==16 && sizeof(GemSwFinish)==4 && sizeof(GemCaching)==8 && sizeof(GemRw)==32,"i915 legacy GEM ABI");
// Per-execbuffer limit of the Brook worker (65536 pages through 512 page
// tables): reported as the aperture so libdrm flushes batches before it.
constexpr uint64_t BdwApertureBytes=uint64_t{65536}*4096;
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
// One bounded step of a CPU-side GPU wait. The caller's interruptible sleep
// (at most 1 ms, returning false on a signal) is kept as the timeout and
// interrupt path; arming on the Retire event only ends it early when a fence
// completes, buffer use is released or a sync object is published. seen
// must be read before the caller tests its condition, so a retirement in
// between is reported as stale and re-checked instead of slept through.
static bool EventWait(const BdwI915UserAccess& u,BdwGpuEvent event,uint64_t seen,uint64_t ns){
    BdwGpuWaiter waiter{};
    const auto armed=BdwGpuEventArm(event,seen,waiter);
    if(armed==BdwWaitArm::Stale)return true;
    const bool ok=u.sleepNs(u.context,ns);
    if(armed==BdwWaitArm::Armed)BdwGpuEventDisarm(waiter);
    return ok;
}
static bool RetireWait(const BdwI915UserAccess& u,uint64_t seen,uint64_t ns){return EventWait(u,BdwGpuEvent::Retire,seen,ns);}
// A full queue that frees no slot for this long is wedged, not busy: report
// it (EBUSY, as before) rather than block the client forever.
static constexpr uint64_t QueueStallNs=10'000'000'000ull;
// Linux i915 blocks an execbuffer until it can be queued; Mesa treats any
// other failure as fatal. Wait for a queue slot before anything is decoded
// or locked: no snapshot exists yet, so an interrupted wait discards nothing.
// EINTR restarts under SA_RESTART (and Mesa's drmIoctl retries it).
static int64_t BeginExecWaitingForSpace(const BdwI915UserAccess& u,BdwSubmissionPreparation& preparation){
    uint64_t progressSeen=0,progressAt=0;bool waited=false;
    for(;;){
        const uint64_t seen=BdwGpuEventGeneration(BdwGpuEvent::Space);
        const auto started=u.beginExec(u.context,u.submitterIncarnation,preparation);
        if(started!=-11)return started;
        if(!u.sleepNs || !u.nowNs){u.endExec(u.context,preparation);BdwNoteRefusal("submit: exec queue full (32 entries)");return -16;}
        const uint64_t now=u.nowNs(u.context);
        if(!waited || seen!=progressSeen){waited=true;progressSeen=seen;progressAt=now;}
        else if(now-progressAt>=QueueStallNs){
            u.endExec(u.context,preparation);BdwNoteRefusal("submit: exec queue full and freed no slot for 10 s");return -16;
        }
        if(!EventWait(u,BdwGpuEvent::Space,seen,1000000)){u.endExec(u.context,preparation);return -4;}
    }
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
    // I915_EXEC_BSD (2): VCS0 user submission (VA-API phase 1c). Admitted only
    // while the VCS executor is ready (gate on, a self-test passed this boot,
    // not quarantined); otherwise refused with a recorded reason. The worker
    // re-checks readiness for every job it runs.
    using R=BdwExecReject;
    // Each check records its own reason, value and limit (see
    // intel_bdw_refusal.h); the order matches the former combined test.
    if(engine==2 && !BdwVideoUserSubmissionReady()){BdwNoteExecReject(R::VideoEngine,engine,2);return invalid;}
    const uint64_t supported=noReloc|handleLut|batchFirst|fenceArray|useExtensions|uint64_t{63};
    const uint32_t maxObjects=BdwGpuFileMaxHandles();
    // A file holds at most maxObjects distinct handles and duplicates are
    // refused (as in Linux), so a longer list can never be valid.
    if(!request.count || request.count>maxObjects){BdwNoteExecReject(R::ObjectCount,request.count,maxObjects);return invalid;}
    if(!request.objects){BdwNoteExecReject(R::ObjectsPointer);return invalid;}
    if(request.flags&~supported){BdwNoteExecReject(R::Flags,request.flags,supported);return invalid;}
    if(!(request.flags&noReloc)){BdwNoteExecReject(R::NoRelocMissing,request.flags,noReloc);return invalid;}
    if(engine>2){BdwNoteExecReject(R::Engine,engine,2);return invalid;}
    if(request.dr1 || request.dr4){BdwNoteExecReject(R::LegacyFields,request.dr1|uint64_t(request.dr4)<<32,0);return invalid;}
    if(request.context>0xffffffffull){BdwNoteExecReject(R::ContextId,request.context,0xffffffffull);return invalid;}
    if(request.fences){BdwNoteExecReject(R::InOutFence,request.fences,0);return invalid;}
    if(request.flags&useExtensions){
        if(!plan || (request.flags&fenceArray) || request.clipCount || !request.clips){
            BdwNoteExecReject(R::ExtensionShape,request.clipCount,0);return invalid;}
    } else if(request.flags&fenceArray){
        if(!plan || !request.clipCount || request.clipCount>BdwMaxSubmissionFences || !request.clips){
            BdwNoteExecReject(R::FenceArrayShape,request.clipCount,BdwMaxSubmissionFences);return invalid;}
    } else if(request.clipCount || request.clips){BdwNoteExecReject(R::Cliprects,request.clipCount,0);return invalid;}
    if((request.start&7) || (request.length&7)){
        BdwNoteExecReject(R::BatchAlignment,(request.start&7)?request.start:request.length,8);return invalid;}
    const uint64_t bytes=uint64_t(request.count)*sizeof(ExecObject);
    if(request.objects>~uint64_t{0}-(bytes-1)){BdwNoteExecReject(R::ObjectsFault,request.objects,bytes);return fault;}
    auto* raw=static_cast<ExecObject*>(kmalloc(bytes));
    if(!raw){BdwNoteExecReject(R::NoMemory,bytes);return nomem;}
    auto* objects=static_cast<BdwExecObjectInput*>(kmalloc(uint64_t(request.count)*sizeof(BdwExecObjectInput)));
    if(!objects){kfree(raw);BdwNoteExecReject(R::NoMemory,uint64_t(request.count)*sizeof(BdwExecObjectInput));return nomem;}
    int64_t result=0;
    if(!u.read(u.context,raw,request.objects,bytes)){result=fault;BdwNoteExecReject(R::ObjectsFault,request.objects,bytes);}
    else {
        for(uint32_t i=0;i<request.count;++i){const auto& obj=raw[i];
            if(obj.relocCount || obj.relocs){result=invalid;BdwNoteExecReject(R::Relocations,obj.relocCount,0,i);break;}
            if(obj.reserved1 || obj.reserved2){result=invalid;BdwNoteExecReject(R::ObjectReserved,obj.reserved1|obj.reserved2,0,i);break;}
            objects[i]={obj.handle,obj.offset,obj.alignment,obj.flags};
        }
        if(!result){
            const uint32_t batchIndex=(request.flags&batchFirst)?0:request.count-1;
            *out=BdwExecSnapshotCreate(file,objects,request.count,batchIndex,request.start,request.length);
            if(!*out){result=invalid;
                // Create records the specific check; ENOMEM is reported as such.
                BdwRefusalDetail d{};if(BdwRefusalDetailFor(g_bdwRefusal,d) && d.why==BdwExecRejectName(R::NoMemory))result=nomem;}
            else {
                BdwExecSnapshotSetOrigin(*out,u.submitterPid,uint32_t(request.context),u.submitterIncarnation);
                if(engine==2 && !BdwExecSnapshotSetEngine(*out,BdwExecEngineVideo)){BdwExecSnapshotRelease(*out,false);*out=nullptr;result=invalid;
                    BdwNoteExecReject(R::Engine,engine,2);}
            }
            if(!result && (request.context || u.renderExperiment)){
                auto* context=request.context?BdwGpuFileGetContext(file,uint32_t(request.context)):
                    BdwGpuFileGetDefaultContext(file);
                if(!context){result=-2;BdwNoteExecReject(R::Context,request.context,0);}
                else {if(!BdwExecSnapshotBindContext(*out,context)){result=invalid;BdwNoteExecReject(R::Context,request.context,1);}
                    BdwGpuContextUnref(context);}
                if(result){BdwExecSnapshotRelease(*out,false);*out=nullptr;}
            }
        }
    }
    if(!result && (request.flags&(fenceArray|useExtensions))){
        ExecFence fences[BdwMaxSubmissionFences]{};uint64_t points[BdwMaxSubmissionFences]{};uint32_t count=request.clipCount;uint64_t handles=request.clips,values=0;
        if(request.flags&useExtensions){
            TimelineExtension ext{};
            if(!u.read(u.context,&ext,request.clips,sizeof(ext))){result=fault;BdwNoteExecReject(R::FenceFault,request.clips,sizeof(ext));}
            else if(ext.next || ext.name || ext.flags || ext.reserved[0] || ext.reserved[1] || ext.reserved[2] || ext.reserved[3] ||
                    !ext.count || ext.count>BdwMaxSubmissionFences || !ext.handles || !ext.points){
                result=invalid;BdwNoteExecReject(R::FenceChain,ext.count,BdwMaxSubmissionFences);}
            else {count=uint32_t(ext.count);handles=ext.handles;values=ext.points;}
        }
        const uint64_t fenceBytes=uint64_t(count)*sizeof(ExecFence),pointBytes=uint64_t(count)*sizeof(uint64_t);
        if(!result && (handles>~uint64_t{0}-(fenceBytes-1) || !u.read(u.context,fences,handles,fenceBytes))){
            result=fault;BdwNoteExecReject(R::FenceFault,handles,fenceBytes);}
        if(!result && values && (values>~uint64_t{0}-(pointBytes-1) || !u.read(u.context,points,values,pointBytes))){
            result=fault;BdwNoteExecReject(R::FenceFault,values,pointBytes);}
        if(!result)for(uint32_t i=0;i<count && !result;++i){
            const auto entry=fences[i];
            if(!entry.handle || !entry.flags || (entry.flags&~3u)){result=invalid;BdwNoteExecReject(R::FenceEntry,entry.flags,3,i);break;}
            auto* object=BdwGpuFileGetSync(file,entry.handle);if(!object){result=-2;BdwNoteExecReject(R::FenceSync,entry.handle,0,i);break;}
            auto* previous=plan->previous(object);
            const uint32_t index=plan->count++;plan->objects[index]=object;plan->timeline[index]=points[i]!=0;
            if(entry.flags&1){auto* wait=points[i]?BdwGpuSyncFindTimeline(object,points[i]):BdwGpuSyncAcquire(object);
                if(!wait){result=invalid;BdwNoteExecReject(R::FenceWait,entry.handle,points[i],i);}
                else {if(!BdwExecSnapshotAddWaitFence(*out,wait)){result=invalid;BdwNoteExecReject(R::FenceWait,entry.handle,BdwMaxSubmissionFences,i);}
                    BdwFenceUnref(wait);}}
            if(!result && (entry.flags&2)){auto* signal=points[i]?(previous?BdwFencePrepareTimelineAfter(previous,points[i]):BdwGpuSyncPrepareTimeline(object,points[i])):BdwFenceCreate();
                if(!signal){result=nomem;BdwNoteExecReject(R::FenceSignal,entry.handle,points[i],i);}
                else {plan->signals[index]=signal;if(!BdwExecSnapshotAddSignalFence(*out,signal)){result=invalid;BdwNoteExecReject(R::FenceSignal,entry.handle,points[i],i);}}}
        }
        if(result){BdwExecSnapshotRelease(*out,false);*out=nullptr;plan->release();}
    }
    kfree(objects);kfree(raw);return result;
}
int64_t BdwIntelCopyExecSnapshot(BdwGpuFile* file,uint64_t arg,const BdwI915UserAccess& u,BdwExecSnapshot** out){
    return CopyExecSnapshot(file,arg,u,out,nullptr);
}
BdwIoctlTrace& BdwIoctlTraceState(){static BdwIoctlTrace trace;return trace;}
static int64_t RenderIoctl(BdwGpuFile* file,uint64_t cmd,uint64_t arg,const BdwI915UserAccess& u);
// Traced entry: records the ioctl, the GETPARAM id or execbuffer flags, and the result.
int64_t BdwIntelRenderIoctl(BdwGpuFile* file,uint64_t cmd,uint64_t arg,const BdwI915UserAccess& u){
    const int64_t result=RenderIoctl(file,cmd,arg,u);
    auto& trace=BdwIoctlTraceState();
    if(__atomic_load_n(&trace.enabled,__ATOMIC_ACQUIRE) && ((cmd>>8)&255)=='d'){
        const uint32_t nr=uint32_t(cmd&255),size=uint32_t((cmd>>16)&0x3fff);uint64_t detail=0;
        if(nr==0x46 && size==16 && u.read){int32_t param=0;if(u.read(u.context,&param,arg,4))detail=uint32_t(param);}
        else if(nr==0x69 && size>=48 && u.read){uint64_t flags=0;if(u.read(u.context,&flags,arg+40,8))detail=flags;}
        uint32_t pid=0;
#if !defined(BROOK_HOST_TEST)
        if(const auto* p=ProcessCurrent())pid=p->pid;
#endif
        trace.record(pid,nr,size,detail,result);
    }
    return result;
}
static int64_t RenderIoctl(BdwGpuFile* file,uint64_t cmd,uint64_t arg,const BdwI915UserAccess& u){
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
        // HAS_BSD (10): 1 only while the VCS0 user-job executor is ready (VA-API
        // phase 1c: gate on and a self-test passed this boot). Otherwise 0, so
        // libva/i965 treat the engine as absent instead of seeing an error.
        case 10: value=BdwVideoUserSubmissionReady()?1:0;break;
        // VA-API phase 2a: truthful answers for libdrm_intel and i965.
        case 5:  value=1;break;  // HAS_GEM
        case 6:  value=0;break;  // NUM_FENCES_AVAIL: no fence registers; tiling is metadata only
        case 11: value=0;break;  // HAS_BLT: BCS is kernel-owned, I915_EXEC_BLT is refused
        case 12: value=1;break;  // HAS_RELAXED_FENCING (no fences to constrain)
        case 15: value=1;break;  // HAS_RELAXED_DELTA
        case 16: value=0;break;  // HAS_GEN7_SOL_RESET: the flag is refused
        case 17: value=1;break;  // HAS_LLC (Broadwell GT2)
        case 18: value=3;break;  // HAS_ALIASING_PPGTT: per-job 48-bit PPGTT
        case 20: value=0;break;  // HAS_SEMAPHORES
        case 22: value=0;break;  // HAS_VEBOX: VECS is not offered to userspace
        case 23: value=0;break;  // HAS_SECURE_BATCHES
        case 27: value=0;break;  // HAS_WT
        case 30: value=1;break;  // MMAP_VERSION: GEM_MMAP accepts the WC flag (WB coherent on LLC)
        case 31: value=0;break;  // HAS_BSD2: one VCS on GT2
        case 33: value=3;break;  // SUBSLICE_TOTAL (BDW GT2: 1 slice x 3 subslices)
        case 34: value=24;break; // EU_TOTAL (HD Graphics 5500: 3 x 8)
        case 38: value=0;break;  // HAS_POOLED_EU
        case 39: value=0;break;  // MIN_EU_IN_POOL
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
        // CPU (1) or GTT (0x40): Brook's only CPU view of a buffer is the
        // linear write-back mapping, so both take the same CPU transition.
        if((d.read!=1 && d.read!=0x40) || (d.write!=0 && d.write!=d.read))return invalid;
        auto* buffer=BdwGpuFileGetBuffer(file,d.handle);if(!buffer)return noent;
        if(!u.sleepNs){BdwGpuBufferUnref(buffer);return notty;}
        int64_t result=0;
        for(;;){const uint64_t seen=BdwGpuEventGeneration(BdwGpuEvent::Retire);const auto attempt=BdwGpuBufferTryCpuTransition(buffer);
            if(attempt==BdwCpuTransitionResult::Ready)break;
            if(attempt==BdwCpuTransitionResult::Failed){result=-5;break;}
            if(!RetireWait(u,seen,1000000)){result=-4;break;}
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
            const uint64_t seen=BdwGpuEventGeneration(BdwGpuEvent::Retire);
            bool all=true,any=false,missing=false;uint32_t first=0;
            for(uint32_t i=0;i<request.count;++i){
                if(!fences[i])fences[i]=BdwGpuSyncAcquire(objects[i]);
                if(!fences[i]){missing=true;all=false;continue;}
                if((request.flags&4) || BdwFenceObservedState(fences[i])){if(!any)first=i;any=true;}else all=false;
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
            if(!RetireWait(u,seen,remaining<1000000?remaining:1000000)){result=-4;break;}
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
            const uint64_t seen=BdwGpuEventGeneration(BdwGpuEvent::Retire);
            bool all=true,any=false,missing=false;uint32_t first=0;
            for(uint32_t i=0;i<request.count;++i){
                if(!fences[i])fences[i]=points[i]?BdwGpuSyncFindTimeline(objects[i],points[i]):BdwGpuSyncAcquire(objects[i]);
                if(!fences[i]){missing=true;all=false;continue;}
                if((request.flags&4) || BdwFenceObservedState(fences[i])){if(!any)first=i;any=true;}else all=false;
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
            if(!RetireWait(u,seen,remaining<1000000?remaining:1000000)){result=-4;break;}
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
            const uint64_t seen=BdwGpuEventGeneration(BdwGpuEvent::Retire);
            fence=request.sourcePoint?BdwGpuSyncFindTimeline(source,request.sourcePoint):BdwGpuSyncAcquire(source);
            if(fence)break;
            if(!(request.flags&2)){result=invalid;break;}
            const uint64_t now=u.nowNs(u.context);if(now>=deadline){result=-62;break;}
            const uint64_t remaining=deadline-now;
            if(!RetireWait(u,seen,remaining<1000000?remaining:1000000)){result=-4;break;}
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
        if(loss)BdwNoteRefusal("reset stats: context reports lost batches (Mesa marks the device lost)");
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
            const uint64_t seen=BdwGpuEventGeneration(BdwGpuEvent::Retire);
            uint32_t uses=0;bool failed=false;BdwGpuBufferGpuStatus(buffer,uses,failed);
            if(failed){result=-5;break;} // EIO: failed/uncertain GPU use
            if(!uses)break;
            now=u.nowNs(u.context);if(now<start){result=-5;break;}
            if(original>=0 && (original==0 || now>=deadline)){result=-62;break;} // ETIME
            const uint64_t wait=original<0?1000000ull:(deadline-now<1000000ull?deadline-now:1000000ull);
            if(!RetireWait(u,seen,wait)){result=-4;break;} // EINTR
        }
        now=u.nowNs(u.context);
        if(original>0)w.timeout=now<start?0:now>=deadline?0:static_cast<int64_t>(deadline-now);
        BdwGpuBufferUnref(buffer);
        return u.write(u.context,arg,&w,sizeof(w))?result:fault;
    }
    // ---- VA-API phase 2a: legacy GEM ioctls used by libdrm_intel / i965 ----
    if(nr==0x5e && size==sizeof(GemMmap)){
        // CPU mmap through the same token mapping as MMAP_OFFSET. The WC flag
        // (1) is accepted and served write-back: Broadwell's LLC keeps the
        // mapping coherent and every GPU use is cache-synchronized anyway.
        GemMmap m{};if(!u.read(u.context,&m,arg,sizeof(m)) || !u.writable(u.context,arg,sizeof(m)))return fault;
        if(m.pad || (m.flags&~uint64_t{1}) || !m.size || (m.offset&4095))return invalid;
        if(!u.mapBuffer)return notty;
        auto* buffer=BdwGpuFileGetBuffer(file,m.handle);if(!buffer)return noent;
        const uint64_t bytes=BdwGpuBufferBytes(buffer);const bool mappable=BdwGpuBufferAllowsUserMapping(buffer);BdwGpuBufferUnref(buffer);
        if(!mappable || m.offset>=bytes || m.size>bytes-m.offset)return invalid;
        const uint64_t token=BdwGpuFileMapToken(file,m.handle);if(!token)return noent;
        uint64_t address=0;const int64_t mapped=u.mapBuffer(u.context,token+m.offset,m.size,address);
        if(mapped)return mapped;
        m.addr=address;return u.write(u.context,arg,&m,sizeof(m))?0:fault;
    }
    if(nr==0x64 && size==sizeof(GemMmapGtt)){
        // Legacy GTT mmap: Brook has no aperture or fence detiling. A linear
        // buffer's GTT view equals its CPU view, so return the WB token; a
        // tiled one is refused rather than handing out a swizzled layout.
        GemMmapGtt m{};if(!u.read(u.context,&m,arg,sizeof(m)) || !u.writable(u.context,arg,sizeof(m)))return fault;
        if(m.pad)return invalid;
        auto* buffer=BdwGpuFileGetBuffer(file,m.handle);if(!buffer)return noent;
        uint32_t mode=0,stride=0;BdwGpuBufferTiling(buffer,mode,stride);BdwGpuBufferUnref(buffer);
        if(mode)return -19; // ENODEV
        m.offset=BdwGpuFileMapToken(file,m.handle);if(!m.offset)return noent;
        return u.write(u.context,arg,&m,sizeof(m))?0:fault;
    }
    if(nr==0x61 && size==sizeof(GemTiling)){
        GemTiling t{};if(!u.read(u.context,&t,arg,sizeof(t)) || !u.writable(u.context,arg,sizeof(t)))return fault;
        if(t.mode>2)return invalid;
        if(t.mode==0)t.stride=0;
        // Gen7+ limits: X tiles are 512 bytes wide, Y tiles 128; stride <= 256 KiB.
        else if(!t.stride || t.stride>256*1024 || (t.stride&(t.mode==1?511u:127u)))return invalid;
        auto* buffer=BdwGpuFileGetBuffer(file,t.handle);if(!buffer)return noent;
        const bool ok=BdwGpuBufferSetTiling(buffer,t.mode,t.stride);BdwGpuBufferUnref(buffer);
        if(!ok)return invalid;
        t.swizzle=0; // I915_BIT_6_SWIZZLE_NONE on Gen8
        return u.write(u.context,arg,&t,sizeof(t))?0:fault;
    }
    if(nr==0x62 && size==sizeof(GemGetTiling)){
        GemGetTiling t{};if(!u.read(u.context,&t,arg,sizeof(t)) || !u.writable(u.context,arg,sizeof(t)))return fault;
        auto* buffer=BdwGpuFileGetBuffer(file,t.handle);if(!buffer)return noent;
        uint32_t mode=0,stride=0;BdwGpuBufferTiling(buffer,mode,stride);BdwGpuBufferUnref(buffer);
        t.mode=mode;t.swizzle=0;t.physSwizzle=0;
        return u.write(u.context,arg,&t,sizeof(t))?0:fault;
    }
    if(nr==0x63 && size==sizeof(GemAperture)){
        if(!u.writable(u.context,arg,sizeof(GemAperture)))return fault;
        const GemAperture a{BdwApertureBytes,BdwApertureBytes};
        return u.write(u.context,arg,&a,sizeof(a))?0:fault;
    }
    if(nr==0x60 && size==sizeof(GemSwFinish)){
        // Only flushes scanout on Linux; Brook's presentation owns that path.
        GemSwFinish f{};if(!u.read(u.context,&f,arg,sizeof(f)))return fault;
        auto* buffer=BdwGpuFileGetBuffer(file,f.handle);if(!buffer)return noent;
        BdwGpuBufferUnref(buffer);return 0;
    }
    if((nr==0x6f || nr==0x70) && size==sizeof(GemCaching)){
        // Every Brook buffer is mapped write-back LLC-coherent for the CPU and
        // the GPU. SET accepts the i915 levels (NONE, CACHED, DISPLAY) without
        // changing that; GET reports the effective level, CACHED.
        GemCaching c{};if(!u.read(u.context,&c,arg,sizeof(c)))return fault;
        if(nr==0x6f && c.caching>2)return invalid;
        auto* buffer=BdwGpuFileGetBuffer(file,c.handle);if(!buffer)return noent;
        BdwGpuBufferUnref(buffer);
        if(nr==0x6f)return 0;
        if(!u.writable(u.context,arg,sizeof(c)))return fault;
        c.caching=1;return u.write(u.context,arg,&c,sizeof(c))?0:fault;
    }
    if((nr==0x5c || nr==0x5d) && size==sizeof(GemRw)){
        // PREAD (0x5c) / PWRITE (0x5d): copy while holding the buffer's CPU
        // transition, so no GPU use can begin mid-copy; waits for current
        // GPU use like SET_DOMAIN. A failed GPU use reports EIO.
        GemRw r{};if(!u.read(u.context,&r,arg,sizeof(r)))return fault;
        if(r.pad)return invalid;
        if(!r.size)return 0;
        if(!u.sleepNs)return notty;
        auto* buffer=BdwGpuFileGetBuffer(file,r.handle);if(!buffer)return noent;
        const uint64_t bytes=BdwGpuBufferBytes(buffer);
        if(!BdwGpuBufferAllowsUserMapping(buffer) || r.offset>=bytes || r.size>bytes-r.offset ||
           r.data>~uint64_t{0}-(r.size-1)){BdwGpuBufferUnref(buffer);return r.offset>=bytes || r.size>bytes-r.offset?invalid:fault;}
        int64_t result=0;
        for(;;){const uint64_t seen=BdwGpuEventGeneration(BdwGpuEvent::Retire);
            if(BdwGpuBufferBeginCpuTransition(buffer))break;
            if(BdwGpuBufferGpuFailed(buffer)){result=-5;break;}
            if(!RetireWait(u,seen,1000000)){result=-4;break;}
        }
        if(!result){
            if(!BdwGpuBufferSyncCpuCache(buffer))result=-5;
            for(uint64_t done=0;!result && done<r.size;){
                const uint64_t at=r.offset+done,page=at/4096,within=at%4096;
                const uint64_t n=(4096-within)<(r.size-done)?(4096-within):(r.size-done);
                const uint64_t kva=BdwGpuBufferPageKernelAddress(buffer,uint32_t(page));
                if(!kva){result=invalid;break;}
                auto* kernel=reinterpret_cast<uint8_t*>(kva)+within;
                const bool ok=nr==0x5d?u.read(u.context,kernel,r.data+done,n):u.write(u.context,r.data+done,kernel,n);
                if(!ok){result=fault;break;}
                done+=n;
            }
            if(!BdwGpuBufferEndCpuTransition(buffer))__builtin_trap();
        }
        BdwGpuBufferUnref(buffer);return result;
    }
    if(nr==0x5b && size==sizeof(GemCreate)){
        GemCreate c{};if(!u.read(u.context,&c,arg,sizeof(c)) || !u.writable(u.context,arg,sizeof(c)))return fault;
        if(c.pad || !c.size || c.size>(uint64_t{65536}<<12))return invalid;
        uint64_t rounded=0;const uint32_t handle=BdwGpuFileCreateBuffer(file,c.size,rounded,
            u.submitterTgid,u.submitterIncarnation);
        if(!handle)return nomem;
        c.handle=handle;c.size=rounded;
        if(!u.write(u.context,arg,&c,sizeof(c))){BdwGpuFileCloseBuffer(file,handle);return fault;}
        return 0;
    }
    if(nr==0x64 && size==sizeof(MmapOffset)){
        MmapOffset m{};if(!u.read(u.context,&m,arg,sizeof(m)) || !u.writable(u.context,arg,sizeof(m)))return fault;
        // WB (2) and WC (1). iris maps shared/scanout buffers (its uncached
        // heap: every tiled Xwayland pixmap) WC; like legacy GEM_MMAP, WC is
        // served write-back, coherent through Broadwell's LLC.
        if(m.pad || m.extensions || (m.flags!=1 && m.flags!=2))return invalid;
        m.offset=BdwGpuFileMapToken(file,m.handle);if(!m.offset)return noent;
        return u.write(u.context,arg,&m,sizeof(m))?0:fault;
    }
    if(nr==0x69 && size==sizeof(ExecBuffer)){
        if(!u.submit)return notty;
        if(bool(u.beginExec)!=bool(u.endExec))return invalid;
        BdwSubmissionPreparation preparation{};
        if(u.beginExec){const auto started=BeginExecWaitingForSpace(u,preparation);
            if(started){if(started!=-4 && !g_bdwRefusal)BdwNoteRefusal("prepare: submission worker not ready or admission refused");return started;}}
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
        (void)BdwTakeRefusal(); // decode reasons below are this call's own
        const int64_t decoded=CopyExecSnapshot(file,arg,u,&snapshot,&plan);
 #if defined(BROOK_GPU_SUBMISSION_TIMING)
        BdwTimingOwnerElapsed(u.submitterIncarnation,BdwTimingSnapshotBuild,admissionAcquired,BdwTimingNow());
 #endif
        // Every decode check records its own reason; this generic one is
        // only a backstop for a path that forgot to.
        if(decoded){if(!g_bdwRefusal)BdwNoteRefusal("execbuffer decode: object, relocation, fence or context validation");return decoded;}
        // The worker can map at most BdwExecMaxPages pages (decode refuses
        // more) through 512 page tables. A sparser buffer set is a normal
        // application request the kernel cannot serve: refuse it here with
        // ENOSPC instead of failing later (the worker also refuses it).
        {uint32_t tables=0;
            if(!BdwExecSnapshotTableCount(snapshot,tables) || tables>BdwPpgttMaxTables){
                const uint32_t pages=BdwExecSnapshotPageCount(snapshot);
                plan.release();if(!BdwExecSnapshotRelease(snapshot,false))__builtin_trap();
                // value: the set's pages; limit: the table count it exceeds.
                static const char* const why="execbuffer: buffer set needs more than 512 page tables";
                g_bdwRefusalDetail={why,pages,BdwPpgttMaxTables,BdwRefusalNoObject};
                BdwNoteRefusal(why);return -28;
            }}
        // A referenced buffer in a CPU-visibility transition (another process's
        // TRY_CPU_READ flushing it, or a handoff retirement) refuses GPU use
        // only while that lasts - milliseconds for a large window buffer.
        // Refusing would become VK_ERROR_DEVICE_LOST in Mesa, so wait it out
        // (bounded, about 100 ms) before admission.
        if(u.sleepNs)for(unsigned i=0;i<100 && BdwExecSnapshotInCpuTransition(snapshot);++i)
            if(!u.sleepNs(u.context,1000000))break;
        int64_t submitted=0;
        // Admission consumes the slot reserved for exactly this snapshot.
        __atomic_store_n(&preparation.snapshot,snapshot,__ATOMIC_RELEASE);
        const bool claimed=u.handoffSubmit && u.handoffSubmit(u.context,snapshot,&submitted);
        if(!claimed)submitted=u.submit(u.context,snapshot);
        if(!submitted){plan.publish();plan.release();return 0;}
        __atomic_store_n(&preparation.snapshot,static_cast<BdwExecSnapshot*>(nullptr),__ATOMIC_RELEASE);
        plan.release();
        // A failed callback is contractually unarmed and retains no owner.
        if(!BdwExecSnapshotRelease(snapshot,false))__builtin_trap();
        if(!g_bdwRefusal)BdwNoteRefusal(claimed?"handoff submit refused":"application submit refused");
        return submitted<0?submitted:-5;
    }
    return notty;
}
}

#include "compositor_scene.h"
#include "gpu_scene_versions.h"
#include "compositor.h"
#include "window.h"
#include "process.h"
#include "scheduler.h"
#include "sync/kmutex.h"
#include "memory/heap.h"
#include "memory/virtual_memory.h"
#include "memory/user_mapping.h"
#include "user_copy.h"
#include <stddef.h>

#ifdef BROOK_HOST_TEST
#include <cstring>
#include <new>
#else
#include "string.h"
inline void* operator new(size_t,void* storage) noexcept { return storage; }
#endif

namespace brook {
extern volatile uint64_t g_lapicTickCount;
namespace {
constexpr uint32_t VersionSlots=64,MaxJobs=64,MaxScenes=32;
constexpr uint64_t PixelLimit=256ull*1024*1024,SnapshotLimit=256ull*1024*1024;
struct Scene {
    Scene* next=nullptr;
    WmCaptureToken binding{};
    SceneImageVersions<VersionSlots> images{PixelLimit};
    uint32_t jobs=0;
    bool closing=false,trim=false;
    // bit 0: a short publication transaction was admitted; bit 1: last handle
    // closed. Their atomic order is the close/publication linearization point.
    uint32_t lifecycle=0;
};
enum class JobKind { Upload,Capture,Present,Clone };
enum class JobState { Preparing,Queued,Running,Done };
struct SceneJob {
    SceneJob* next;
    Scene* scene;
    Scene* sourceScene; // Clone input owns a separate scene job reference.
    uint32_t references;
    JobKind kind;
    JobState state;
    bool canceled;
    int64_t result;
    SceneImageInfo image;
    uint64_t snapshot,pages;
    MappingBacking* sourceLease;
    int32_t producer;
    uint32_t resource,sourceFormat,count;
    uint64_t versions[GPU_SCENE_MAX_LAYERS];
    GpuSceneLayer layers[GPU_SCENE_MAX_LAYERS];
    WmCaptureToken token;
};
KMutex g_sceneLock{};
Scene* g_scenes;
Scene* g_bindings[WM_MAX_WINDOWS]{}; // Non-owning; only cleared when matching.
SceneJob* g_head;
SceneJob* g_tail;
SceneJob* g_quarantine;
uint32_t g_jobs,g_sceneCount;
uint64_t g_snapshotBytes;

void ApplyClose(Scene* scene) {
    if (scene->closing || !(__atomic_load_n(&scene->lifecycle,__ATOMIC_ACQUIRE)&2u)) return;
    scene->closing=true;scene->images.Close();
    if (g_bindings[scene->binding.wmId-1]==scene) g_bindings[scene->binding.wmId-1]=nullptr;
}
bool BeginPublication(Scene* scene) {
    uint32_t expected=0;
    return __atomic_compare_exchange_n(&scene->lifecycle,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
void EndPublication(Scene* scene) {
    __atomic_fetch_and(&scene->lifecycle,~1u,__ATOMIC_RELEASE);ApplyClose(scene);
}
void JobUnref(SceneJob* job) {
    if (__atomic_fetch_sub(&job->references,1,__ATOMIC_ACQ_REL)!=1) return;
    if (job->snapshot) VmmFreePages(VirtualAddress(job->snapshot),job->pages);
    if (job->sourceLease) MappingBackingUnref(job->sourceLease);
    KMutexLock(&g_sceneLock);
    if (job->sourceScene) --job->sourceScene->jobs;
    --job->scene->jobs;--g_jobs;g_snapshotBytes-=job->pages*4096;
    KMutexUnlock(&g_sceneLock);kfree(job);CompositorWake();
}
SceneJob* NewJob(Scene* scene,JobKind kind) {
    auto* job=static_cast<SceneJob*>(kmalloc(sizeof(SceneJob)));
    if (!job) return nullptr;
    memset(job,0,sizeof(*job));job->scene=scene;job->references=1;
    job->kind=kind;job->state=JobState::Preparing;return job;
}
// Called under g_sceneLock before any user pixels are copied or GPU work starts.
int Admit(SceneJob* job,uint64_t pages) {
    ApplyClose(job->scene);
    if (job->scene->closing) return -125;
    if (g_jobs>=MaxJobs || pages>SnapshotLimit/4096 ||
        g_snapshotBytes>SnapshotLimit-pages*4096) return -11;
    ++g_jobs;++job->scene->jobs;job->pages=pages;g_snapshotBytes+=pages*4096;return 0;
}
int ReserveImageLocked(SceneJob* job,uint32_t w,uint32_t h,uint32_t format,uint64_t pages) {
    int error=Admit(job,pages);
    if (!error) {
        const auto result=job->scene->images.Reserve(w,h,format,job->image);
        if (result!=SceneReserveResult::Ready) {
            --g_jobs;--job->scene->jobs;g_snapshotBytes-=pages*4096;job->pages=0;
            error=result==SceneReserveResult::Invalid?-22:
                  result==SceneReserveResult::IdExhausted?-75:
                  result==SceneReserveResult::Closing?-125:-11;
            if (result==SceneReserveResult::Capacity) job->scene->trim=true;
        }
    }
    return error;
}
int ReserveImage(SceneJob* job,uint32_t w,uint32_t h,uint32_t format,uint64_t pages) {
    KMutexLock(&g_sceneLock);const int error=ReserveImageLocked(job,w,h,format,pages);
    KMutexUnlock(&g_sceneLock);if (error) CompositorWake();return error;
}
bool CancelMetadata(SceneJob* job) {
    if (job->kind==JobKind::Present) return job->scene->images.Unpin(job->versions,job->count);
    if (job->kind==JobKind::Clone && !job->sourceScene->images.Unpin(job->versions,1)) return false;
    return job->scene->images.Reject(job->image.version);
}
void QuarantineJob(SceneJob* job,bool hasWorkerReference) {
    if (!hasWorkerReference) __atomic_fetch_add(&job->references,1,__ATOMIC_RELAXED);
    if (job->kind==JobKind::Present)
        for (uint32_t i=0;i<job->count;++i) job->scene->images.Quarantine(job->versions[i]);
    if (job->kind==JobKind::Clone) {
        job->sourceScene->images.Quarantine(job->versions[0]);
        job->scene->images.Quarantine(job->image.version);
    }
    job->state=JobState::Done;job->result=-5;job->next=g_quarantine;g_quarantine=job;
}
// Return false when close/cancellation won while a source snapshot was prepared.
int Enqueue(SceneJob* job) {
    KMutexLock(&g_sceneLock);
    ApplyClose(job->scene);
    if (job->scene->closing) {
        const bool safe=CancelMetadata(job);
        if (!safe) QuarantineJob(job,false);
        KMutexUnlock(&g_sceneLock);return safe?-125:-5;
    }
    __atomic_fetch_add(&job->references,1,__ATOMIC_RELAXED);
    job->state=JobState::Queued;
    if (g_tail) g_tail->next=job;else g_head=job;
    g_tail=job;KMutexUnlock(&g_sceneLock);CompositorWake();return 0;
}
int64_t WaitJob(Process* caller,SceneJob* job) {
    const uint64_t deadline=g_lapicTickCount+5000;
    int64_t result;
    for (;;) {
        bool dropWorker=false;
        KMutexLock(&g_sceneLock);
        if (job->state==JobState::Done) {
            result=job->result;KMutexUnlock(&g_sceneLock);break;
        }
        if (g_lapicTickCount>=deadline ||
            (caller && __atomic_load_n(&caller->terminalPending,__ATOMIC_ACQUIRE))) {
            job->canceled=true;
            if (job->state==JobState::Queued) {
                SceneJob* previous=nullptr;
                for (auto* item=g_head;item;item=item->next) {
                    if (item==job) {
                        if (previous) previous->next=item->next;else g_head=item->next;
                        if (g_tail==item) g_tail=previous;
                        if (CancelMetadata(job)) {
                            job->state=JobState::Done;job->result=-110;dropWorker=true;
                        } else QuarantineJob(job,true);
                        break;
                    }
                    previous=item;
                }
            }
        }
        KMutexUnlock(&g_sceneLock);
        if (dropWorker) JobUnref(job);
        SchedulerSleepMs(1);
    }
    JobUnref(job);return result;
}
// Bounded, compositor-thread-only cleanup. A marked eviction holds the scene
// alive until this worker records the driver result. No callbacks under lock.
void TrimImages(uint32_t budget,bool all=false) {
    const auto* gpu=GpuCompositorGet();if (!gpu) return;
    for (uint32_t n=0;n<budget;++n) {
        Scene* chosen=nullptr;SceneImageInfo image{};uint32_t slot=0;
        KMutexLock(&g_sceneLock);
        for (auto* scene=g_scenes;scene;scene=scene->next) {
            if ((all || scene->closing || scene->trim) && scene->images.BeginEviction(slot,image)) {
                chosen=scene;break;
            }
            if (scene->trim && !scene->closing) scene->trim=false;
        }
        KMutexUnlock(&g_sceneLock);if (!chosen) break;
        const bool retired=gpu->DestroyOwnedTexture(image.texture);
        KMutexLock(&g_sceneLock);chosen->images.EndEviction(slot,image.version,retired);
        KMutexUnlock(&g_sceneLock);
    }
}
void ReapScenes() {
    for (;;) {
        Scene* garbage=nullptr;
        KMutexLock(&g_sceneLock);
        Scene** link=&g_scenes;
        while (*link) {
            auto* scene=*link;
            if (scene->closing && !scene->jobs && scene->images.Drained()) {
                *link=scene->next;--g_sceneCount;garbage=scene;break;
            }
            link=&scene->next;
        }
        KMutexUnlock(&g_sceneLock);if (!garbage) break;
        garbage->~Scene();kfree(garbage);
    }
}
}

struct SceneHandle { uint32_t references;Scene* scene; };

SceneHandle* SceneCreate(Process* caller,uint32_t window,int* error) {
    *error=-95;
    const auto* gpu=GpuCompositorGet();
    if (!CompositorGpuActive() || !gpu || !gpu->UploadOwnedTexture || !gpu->CaptureExternal ||
        !gpu->ComposeOwnedLayers || !gpu->DrawPremultipliedQuad) return nullptr;
    WmCaptureToken binding{};
    if (!WmGetCaptureBinding(caller,window,&binding)) { *error=-2;return nullptr; }
    auto* memory=kmalloc(sizeof(Scene));
    auto* handle=static_cast<SceneHandle*>(kmalloc(sizeof(SceneHandle)));
    if (!memory || !handle) { kfree(memory);kfree(handle);*error=-12;return nullptr; }
    auto* scene=new(memory) Scene;scene->binding=binding;
    KMutexLock(&g_sceneLock);
    if (g_sceneCount>=MaxScenes) {
        KMutexUnlock(&g_sceneLock);scene->~Scene();kfree(scene);kfree(handle);*error=-11;return nullptr;
    }
    scene->next=g_scenes;g_scenes=scene;++g_sceneCount;
    KMutexUnlock(&g_sceneLock);*handle={1,scene};*error=0;return handle;
}
bool SceneActivate(SceneHandle* handle) {
    KMutexLock(&g_sceneLock);
    const auto binding=handle->scene->binding;const auto window=WmSnapshotContent(binding.wmId-1);
    const bool valid=!handle->scene->closing && window.live && window.incarnation==binding.incarnation;
    if (valid) g_bindings[binding.wmId-1]=handle->scene;
    KMutexUnlock(&g_sceneLock);return valid;
}
void SceneHandleRef(SceneHandle* handle) {
    __atomic_fetch_add(&handle->references,1,__ATOMIC_RELAXED);
}
void SceneHandleUnref(SceneHandle* handle) {
    if (__atomic_fetch_sub(&handle->references,1,__ATOMIC_ACQ_REL)!=1) return;
    // SCM queue cleanup may drop the last reference under a socket lock. Do
    // not acquire a sleeping scene mutex or touch the GPU on that path. The
    // global scene list retains the object until its worker acknowledges close.
    __atomic_fetch_or(&handle->scene->lifecycle,2u,__ATOMIC_RELEASE);
    kfree(handle);CompositorWake();
}
void SceneGetStats(SceneHandle* handle,SceneStatsDesc& out) {
    KMutexLock(&g_sceneLock);const auto stats=handle->scene->images.Stats();
    out={SCENE_ABI_VERSION,GPU_SCENE_MAX_LAYERS,GPU_SCENE_MAX_DIMENSION,VersionSlots,
        stats.bytes,stats.byteLimit,stats.pinned,g_snapshotBytes,SnapshotLimit,
        stats.published,stats.reserved,stats.idle,stats.quarantined,g_jobs,MaxJobs,g_sceneCount,MaxScenes};
    KMutexUnlock(&g_sceneLock);
}
int64_t SceneUpload(Process* caller,SceneHandle* handle,const SceneUploadDesc& desc) {
    if (!desc.width || !desc.height || desc.width>GPU_SCENE_MAX_DIMENSION || desc.height>GPU_SCENE_MAX_DIMENSION ||
        (desc.format!=1 && desc.format!=2) || uint64_t(desc.width)*4>desc.stride) return -22;
    const uint64_t packed=uint64_t(desc.width)*desc.height*4;
    const uint64_t extent=uint64_t(desc.height-1)*desc.stride+uint64_t(desc.width)*4;
    if (!desc.pixels || extent>desc.bytes || desc.pixels>~uint64_t{0}-extent) return -22;
    if (packed>SnapshotLimit) return -7;
    auto* job=NewJob(handle->scene,JobKind::Upload);if (!job) return -12;
    const int error=ReserveImage(job,desc.width,desc.height,desc.format,(packed+4095)/4096);
    if (error) { kfree(job);return error; }
    const auto snapshot=VmmAllocPages(job->pages,VMM_WRITABLE,MemTag::Device,KernelPid);
    job->snapshot=snapshot.raw();bool copied=bool(snapshot);
    if (copied) for (uint32_t row=0;row<desc.height;++row) {
        if (!CopyFromUser(reinterpret_cast<void*>(snapshot.raw()+uint64_t(row)*desc.width*4),
                          desc.pixels+uint64_t(row)*desc.stride,uint64_t(desc.width)*4)) { copied=false;break; }
    }
    if (!copied) {
        KMutexLock(&g_sceneLock);CancelMetadata(job);KMutexUnlock(&g_sceneLock);
        JobUnref(job);return snapshot?-14:-12;
    }
    const int queued=Enqueue(job);if (queued) { JobUnref(job);return queued; }
    return WaitJob(caller,job);
}
int64_t SceneCapture(Process* caller,SceneHandle* handle,const SceneCaptureDesc& desc,MappingBacking* lease,
                     int32_t producer,uint32_t resource,uint32_t sourceFormat) {
    if (!lease || !desc.width || !desc.height || desc.width>GPU_SCENE_MAX_DIMENSION ||
        desc.height>GPU_SCENE_MAX_DIMENSION || (desc.format!=1 && desc.format!=2) ||
        !GpuCaptureFormatSupported(sourceFormat)) return -22;
    auto* job=NewJob(handle->scene,JobKind::Capture);if (!job) return -12;
    const int error=ReserveImage(job,desc.width,desc.height,desc.format,0);
    if (error) { kfree(job);return error; }
    MappingBackingRef(lease);job->sourceLease=lease;job->producer=producer;
    job->resource=resource;job->sourceFormat=sourceFormat;
    const int queued=Enqueue(job);if (queued) { JobUnref(job);return queued; }
    return WaitJob(caller,job);
}
int64_t ScenePresent(Process* caller,SceneHandle* handle,const ScenePresentDesc& desc,const SceneLayerDesc* layers) {
    if (!desc.width || !desc.height || desc.width>GPU_SCENE_MAX_DIMENSION || desc.height>GPU_SCENE_MAX_DIMENSION ||
        desc.count>GPU_SCENE_MAX_LAYERS || (desc.count && !layers)) return -22;
    auto* scene=handle->scene;
    auto* job=NewJob(scene,JobKind::Present);if (!job) return -12;
    job->count=desc.count;
    KMutexLock(&g_sceneLock);
    int error=0;
    if (g_bindings[scene->binding.wmId-1]!=scene || scene->closing) error=-116;
    for (uint32_t i=0;!error && i<desc.count;++i) {
        const auto& l=layers[i];SceneImageInfo image{};
        if (l.reserved || !scene->images.GetPublished(l.version,image)) { error=-22;break; }
        job->versions[i]=l.version;
        job->layers[i]={image.texture,l.sx,l.sy,l.sw,l.sh,l.x,l.y,l.w,l.h,l.opacity,l.clipX,l.clipY,l.clipW,l.clipH};
        GpuSceneRect rect{};
        if (!GpuSceneClip(job->layers[i],image.width,image.height,desc.width,desc.height,rect)) error=-22;
    }
    if (!error) error=Admit(job,0);
    if (!error && !scene->images.Pin(job->versions,job->count)) {
        --g_jobs;--scene->jobs;error=-22;
    }
    if (!error && !WmReserveBoundCapture(scene->binding,desc.width,desc.height,&job->token)) {
        if (CancelMetadata(job)) { --g_jobs;--scene->jobs;error=-116; }
        else { QuarantineJob(job,false);error=-5; }
    }
    KMutexUnlock(&g_sceneLock);
    if (error) { if (error==-5) JobUnref(job);else kfree(job);return error; }
    const int queued=Enqueue(job);if (queued) { JobUnref(job);return queued; }
    return WaitJob(caller,job);
}
// Copy an already captured immutable image into another scene. The original
// producer may have been released; never reconstruct this from its old GEM ID.
int64_t SceneClone(Process* caller,SceneHandle* destination,SceneHandle* source,uint64_t version) {
    auto* job=NewJob(destination->scene,JobKind::Clone);if (!job) return -12;
    auto* input=source->scene;SceneImageInfo image{};
    KMutexLock(&g_sceneLock);ApplyClose(input);
    int error=0;
    if (!input->images.GetPublished(version,image)) error=-22;
    if (!error) error=ReserveImageLocked(job,image.width,image.height,1,0);
    if (!error && !input->images.Pin(&version,1)) {
        job->scene->images.Reject(job->image.version);--g_jobs;--job->scene->jobs;error=-22;
    }
    if (!error) {
        job->sourceScene=input;++input->jobs;job->versions[0]=version;job->count=1;
        job->layers[0]={image.texture,0,0,image.width,image.height,0,0,image.width,image.height,
                        255,0,0,image.width,image.height};
    }
    KMutexUnlock(&g_sceneLock);
    if (error) { kfree(job);CompositorWake();return error; }
    const int queued=Enqueue(job);if (queued) { JobUnref(job);return queued; }
    return WaitJob(caller,job);
}
int SceneRelease(SceneHandle* handle,uint64_t version) {
    KMutexLock(&g_sceneLock);const bool released=handle->scene->images.Release(version);
    KMutexUnlock(&g_sceneLock);if (released) CompositorWake();return released?0:-22;
}

void CompositorDrainScenes() {
    const auto* gpu=GpuCompositorGet();if (!gpu) return;
    KMutexLock(&g_sceneLock);
    for (auto* scene=g_scenes;scene;scene=scene->next) ApplyClose(scene);
    KMutexUnlock(&g_sceneLock);
    TrimImages(4);
    for (uint32_t n=0;n<8;++n) {
        KMutexLock(&g_sceneLock);auto* job=g_head;
        if (!job) { KMutexUnlock(&g_sceneLock);break; }
        g_head=job->next;if (!g_head) g_tail=nullptr;
        job->next=nullptr;job->state=JobState::Running;
        ApplyClose(job->scene);
        const bool canceled=job->scene->closing || job->canceled;
        bool canceledUncertain=false,inputsReady=true;
        if (canceled) {
            if (CancelMetadata(job)) { job->state=JobState::Done;job->result=-125; }
            else { QuarantineJob(job,true);canceledUncertain=true; }
        } else if (job->kind==JobKind::Present) {
            for (uint32_t i=0;i<job->count;++i) {
                SceneImageInfo image{};
                if (!job->scene->images.GetPinned(job->versions[i],image)) inputsReady=false;
            }
        } else if (job->kind==JobKind::Clone) {
            SceneImageInfo image{};
            inputsReady=job->sourceScene->images.GetPinned(job->versions[0],image);
        }
        KMutexUnlock(&g_sceneLock);
        if (canceled) { if (!canceledUncertain) JobUnref(job);continue; }
        auto* scene=job->scene;GpuCaptureResult result=GpuCaptureResult::Rejected;
        uint32_t texture=job->image.texture;
        if (job->kind==JobKind::Present) result=inputsReady?
            CompositorBuildSceneRoot(job->token,job->layers,job->count):GpuCaptureResult::Uncertain;
        else if (!inputsReady) result=GpuCaptureResult::Uncertain;
        else {
            if (!texture) {
                texture=gpu->CreateOwnedTexture(job->image.width,job->image.height,job->image.format==1);
                if (!texture) {
                    TrimImages(8,true);
                    texture=gpu->CreateOwnedTexture(job->image.width,job->image.height,job->image.format==1);
                }
            }
            if (texture) {
                if (job->kind==JobKind::Upload)
                    result=gpu->UploadOwnedTexture(texture,job->snapshot,job->image.width*job->image.height*4);
                else if (job->kind==JobKind::Clone)
                    result=gpu->ComposeOwnedLayers(texture,job->layers,1);
                else result=gpu->CaptureExternal(texture,job->producer,job->resource,job->sourceFormat,
                                                 job->image.width,job->image.height);
            }
        }
        SceneRetiredRoot retired{};
        bool uncertain=result==GpuCaptureResult::Uncertain;
        KMutexLock(&g_sceneLock);
        ApplyClose(scene);
        const bool discard=job->canceled || scene->closing;
        job->result=-5;
        if (job->kind==JobKind::Clone && !uncertain &&
            !job->sourceScene->images.Unpin(job->versions,1)) uncertain=true;
        if (job->kind==JobKind::Present) {
            if (uncertain) {
                for (uint32_t i=0;i<job->count;++i) scene->images.Quarantine(job->versions[i]);
            } else {
                const bool admitted=result==GpuCaptureResult::Complete && !discard &&
                    g_bindings[scene->binding.wmId-1]==scene && BeginPublication(scene);
                const bool publish=admitted && CompositorPublishSceneRoot(job->token,retired);
                if (admitted) EndPublication(scene);
                scene->images.Unpin(job->versions,job->count);job->result=publish?0:-116;
            }
        } else if (uncertain) scene->images.Quarantine(job->image.version,texture);
        else if (result==GpuCaptureResult::Complete && !discard && BeginPublication(scene)) {
            const auto completed=scene->images.Complete(job->image.version,texture);
            if (completed==SceneCompleteResult::Published) job->result=static_cast<int64_t>(job->image.version);
            else if (completed==SceneCompleteResult::Discarded) job->result=-125;
            else { scene->images.Quarantine(job->image.version,texture);uncertain=true; }
            EndPublication(scene);
        } else {
            scene->images.Reject(job->image.version,texture);job->result=discard?-125:(texture?-22:-12);
        }
        job->state=JobState::Done;
        if (uncertain) QuarantineJob(job,true);
        KMutexUnlock(&g_sceneLock);
        CompositorRetireSceneRoot(retired);
        if (!uncertain) JobUnref(job);
    }
    TrimImages(4);ReapScenes();
}
}

#pragma once
#include <stdint.h>
#include "gpu_scene_abi.h"
#include "gpu_compositor.h"

namespace brook {
struct Process;struct MappingBacking;struct WmCaptureToken;
struct SceneHandle;
SceneHandle* SceneCreate(Process*,uint32_t window,int* error);
bool SceneActivate(SceneHandle*);
void SceneHandleRef(SceneHandle*);
void SceneHandleUnref(SceneHandle*);
void SceneGetStats(SceneHandle*,SceneStatsDesc&);
int64_t SceneUpload(Process*,SceneHandle*,const SceneUploadDesc&);
int64_t SceneCapture(Process*,SceneHandle*,const SceneCaptureDesc&,MappingBacking*,
                     int32_t producer,uint32_t resource,uint32_t sourceFormat);
int64_t ScenePresent(Process*,SceneHandle*,const ScenePresentDesc&,const SceneLayerDesc*);
int64_t SceneClone(Process*,SceneHandle* destination,SceneHandle* source,uint64_t version);
int SceneRelease(SceneHandle*,uint64_t version);
// Compositor-thread only. No GPU operation runs under the metadata lock.
void CompositorDrainScenes();

// Compositor-owned roots outlive scene handles. Publication only swaps metadata;
// the returned old root must be retired after dropping the scene lock.
struct SceneRetiredRoot { uint32_t texture;bool owned; };
GpuCaptureResult CompositorBuildSceneRoot(const WmCaptureToken&,const GpuSceneLayer*,uint32_t);
bool CompositorPublishSceneRoot(const WmCaptureToken&,SceneRetiredRoot&);
void CompositorRetireSceneRoot(const SceneRetiredRoot&);
}

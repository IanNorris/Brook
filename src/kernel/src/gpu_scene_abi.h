#pragma once
#include <stdint.h>

// Wire structures shared with userspace. Backend texture/resource numbers are
// never scene capabilities. Version IDs are scoped to an open scene descriptor.
#ifdef __cplusplus
namespace brook {
#define BROOK_SCENE_SIZE(type,bytes) static_assert(sizeof(type)==bytes)
#else
#define BROOK_SCENE_SIZE(type,bytes) _Static_assert(sizeof(struct type)==bytes,#type)
#endif
enum { SCENE_ABI_VERSION=1, SCENE_CTL_STATS=0, SCENE_CTL_UPLOAD=1,
       SCENE_CTL_CAPTURE=2, SCENE_CTL_PRESENT=3, SCENE_CTL_RELEASE=4, SCENE_CTL_CLONE=5,
       SCENE_CTL_CAPTURE_DMABUF=6 };
struct SceneUploadDesc {
    uint64_t pixels,bytes;
    uint32_t width,height,stride,format; // stride bytes; 1=BGRA premultiplied, 2=BGRX
};
struct SceneCaptureDesc {
    int32_t drmFd;
    uint32_t gem,width,height,format,reserved; // format is destination alpha policy
};
struct SceneCloneDesc {
    int32_t sourceFd;
    uint32_t reserved;
    uint64_t version;
};
struct SceneDmabufCaptureDesc {
    int32_t drmFd;
    uint32_t gem,width,height,fourcc,stride,offset,flags;
    uint64_t modifier;
};
struct SceneLayerDesc {
    uint64_t version;
    uint32_t sx,sy,sw,sh;
    int32_t x,y;
    uint32_t w,h,opacity;
    int32_t clipX,clipY;
    uint32_t clipW,clipH,reserved;
};
struct ScenePresentDesc {
    uint64_t layers;
    uint32_t count,width,height,reserved;
};
struct SceneStatsDesc {
    uint32_t abi,maxLayers,maxDimension,versionSlots;
    uint64_t pixelBytes,pixelByteLimit,pins,snapshotBytes,snapshotByteLimit;
    uint32_t published,reserved,idle,quarantined,queued,maxQueued,sceneCount,maxScenes;
};
BROOK_SCENE_SIZE(SceneUploadDesc,32);
BROOK_SCENE_SIZE(SceneCaptureDesc,24);
BROOK_SCENE_SIZE(SceneLayerDesc,64);
BROOK_SCENE_SIZE(ScenePresentDesc,24);
BROOK_SCENE_SIZE(SceneStatsDesc,88);
BROOK_SCENE_SIZE(SceneCloneDesc,16);
BROOK_SCENE_SIZE(SceneDmabufCaptureDesc,40);
#undef BROOK_SCENE_SIZE
#ifdef __cplusplus
}
#endif

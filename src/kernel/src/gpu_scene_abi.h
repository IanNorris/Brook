#pragma once
#include <stdint.h>

// Wire structures shared with userspace. Backend texture/resource numbers are
// never scene capabilities. Version IDs are scoped to an open scene descriptor.
namespace brook {
inline constexpr uint32_t SCENE_ABI_VERSION=1;
inline constexpr uint32_t SCENE_CTL_STATS=0,SCENE_CTL_UPLOAD=1,SCENE_CTL_CAPTURE=2;
inline constexpr uint32_t SCENE_CTL_PRESENT=3,SCENE_CTL_RELEASE=4;
struct SceneUploadDesc {
    uint64_t pixels,bytes;
    uint32_t width,height,stride,format; // stride bytes; 1=BGRA premultiplied, 2=BGRX
};
struct SceneCaptureDesc {
    int32_t drmFd;
    uint32_t gem,width,height,format,reserved; // format is destination alpha policy
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
static_assert(sizeof(SceneUploadDesc)==32 && sizeof(SceneCaptureDesc)==24);
static_assert(sizeof(SceneLayerDesc)==64 && sizeof(ScenePresentDesc)==24);
static_assert(sizeof(SceneStatsDesc)==88);
}

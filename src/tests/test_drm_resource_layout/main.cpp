#include "../../kernel/src/drm_resource_layout.h"
#include <cassert>

int main()
{
    uint32_t stride = 0, size = 0;
    // Xwayland/Mesa's 800-pixel rows occupy 3328 rather than 3200 bytes.
    assert(DrmLegacyResourceLayout(false,800,600,0,1996800,&stride,&size));
    assert(stride==3200 && size==1996800);
    assert(DrmLegacyResourceLayout(false,800,600,3328,1996800,&stride,&size));
    assert(stride==3328 && size==1996800);
    // A real mip/layer allocation also exceeds the base-level minimum.
    assert(DrmLegacyResourceLayout(false,64,64,0,32768,&stride,&size));
    assert(size==32768);
    assert(DrmLegacyResourceLayout(false,64,64,0,1,&stride,&size));
    assert(size==16384);
    assert(DrmLegacyResourceLayout(true,8192,0,0,1,&stride,&size));
    assert(stride==8192 && size==8192);
    assert(DrmLegacyResourceLayout(true,8192,0,0,16384,&stride,&size));
    assert(size==16384);
    assert(!DrmLegacyResourceLayout(false,16384,16384,UINT32_MAX,1,&stride,&size));
    assert(!DrmLegacyResourceLayout(false,UINT32_MAX,1,0,1,&stride,&size));
    assert(!DrmLegacyResourceLayout(true,0,0,0,0,&stride,&size));
}

#include "../../kernel/src/gpu_scene.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <climits>
#include <cstring>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"GPU_SCENE line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
int main() {
    using namespace brook;
    CHECK(GpuViewportHalfBits(5)==0x40200000u);
    CHECK(GpuViewportHalfBits(511)==0x437f8000u);
    CHECK(GpuViewportHalfBits(512)==0x43800000u);
    CHECK(GpuViewportHalfBits(1280)==0x44200000u);
    CHECK(GpuViewportHalfBits(16384)==0x46000000u);
    for (uint32_t dimension=0;dimension<=GPU_SCENE_MAX_DIMENSION;++dimension) {
        const float half=float(dimension)/2;uint32_t bits;
        std::memcpy(&bits,&half,sizeof(bits));CHECK(GpuViewportHalfBits(dimension)==bits);
    }
    uint32_t random=7;
    auto next=[&](unsigned bound) { random=random*1664525u+1013904223u;return random%bound; };
    for (unsigned trial=0;trial<10000;++trial) {
        const uint32_t sourceW=1+next(16),sourceH=1+next(16),rootW=1+next(19),rootH=1+next(19);
        const uint32_t sx=next(sourceW),sy=next(sourceH);
        GpuSceneLayer l{1,sx,sy,1+next(sourceW-sx),1+next(sourceH-sy),
            int(next(81))-40,int(next(81))-40,1+next(32),1+next(32),next(256),
            int(next(81))-40,int(next(81))-40,next(33),next(33)};
        GpuSceneRect r;CHECK(GpuSceneClip(l,sourceW,sourceH,rootW,rootH,r));
        bool seen=false;
        for (int y=0;y<int(rootH);++y) for (int x=0;x<int(rootW);++x) {
            const bool covered=l.opacity && x>=l.x && y>=l.y && x<int64_t(l.x)+l.w &&
                y<int64_t(l.y)+l.h && x>=l.clipX && y>=l.clipY &&
                x<int64_t(l.clipX)+l.clipW && y<int64_t(l.clipY)+l.clipH;
            CHECK(covered==(r.visible && x>=r.x && x<r.right && y>=r.y && y<r.bottom));
            if (!covered) continue;
            seen=true;
            const double u=(l.sx+(x+.5-l.x)*l.sw/l.w)/sourceW;
            const double v=(l.sy+(y+.5-l.y)*l.sh/l.h)/sourceH;
            const double actualU=(r.u0+(x+.5-r.x)*(r.u1-r.u0)/(r.right-r.x))/r.uDen;
            const double actualV=(r.v0+(y+.5-r.y)*(r.v1-r.v0)/(r.bottom-r.y))/r.vDen;
            CHECK(std::abs(u-actualU)<1e-12 && std::abs(v-actualV)<1e-12);
        }
        CHECK(seen==r.visible);
    }
    GpuSceneLayer edge{1,0,0,16384,16384,0,0,16384,16384,255,0,0,16384,16384};
    GpuSceneRect r;CHECK(GpuSceneClip(edge,16384,16384,16384,16384,r));
    CHECK(r.u1==268435456 && r.uDen==268435456 && r.visible);
    for (int offset:{INT_MIN,INT_MAX}) {
        edge.x=edge.y=offset;edge.clipX=edge.clipY=offset;
        CHECK(GpuSceneClip(edge,16384,16384,16384,16384,r) && !r.visible);
    }
    edge.x=edge.y=edge.clipX=edge.clipY=0;
    edge.sx=UINT_MAX;CHECK(!GpuSceneClip(edge,16384,16384,4,4,r));edge.sx=0;
    edge.opacity=256;CHECK(!GpuSceneClip(edge,16384,16384,4,4,r));edge.opacity=255;
    edge.w=UINT_MAX;CHECK(!GpuSceneClip(edge,16384,16384,4,4,r));edge.w=16384;
    edge.clipW=UINT_MAX;CHECK(!GpuSceneClip(edge,16384,16384,4,4,r));edge.clipW=16384;
    CHECK(!GpuSceneClip(edge,0,4,4,4,r));
    const std::vector<GpuSceneStep> order={GpuSceneStep::UploadVertices,GpuSceneStep::RenderAndFence,GpuSceneStep::RestoreAndFence};
    for (unsigned fail=0;fail<=order.size();++fail) {
        unsigned seen=0;
        const bool complete=GpuSceneSequence([&](auto step) { CHECK(step==order[seen]);return seen++!=fail; });
        CHECK(complete==(fail==order.size()));CHECK(seen==(fail<order.size()?fail+1:order.size()));
    }
    std::puts("GPU_SCENE_PASS randomized clipped/scaled UVs, signed limits, bounds, completion/restore failures");
}
